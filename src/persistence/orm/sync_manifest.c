#include "persistence/orm/sync_manifest.h"
#include "persistence/orm/sync_manifest_models.h"
#include "persistence/orm/records.h"
#include <norm/query.h>
#include <nb/file.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static int manifest_initialize(struct db *db)
{
	static const struct norm_model *const models[] = {
		&orm_sync_entries,
		&orm_sync_conflicts,
		&orm_sync_trash,
		&orm_sync_tombstones,
	};
	static const struct {
		const struct norm_model *model;
		const char *column;
	} columns[] = {
		{&orm_sync_entries, "local_size"},  {&orm_sync_entries, "remote_size"},
		{&orm_sync_entries, "local_mtime"}, {&orm_sync_entries, "remote_mtime"},
		{&orm_sync_entries, "last_device"}, {&orm_sync_conflicts, "resolved_at"},
	};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_schema_options options = {.if_not_exists = true};
	int rc = orm_scope_begin(db, &scope);

	for (size_t i = 0; rc == 0 && i < sizeof(models) / sizeof(models[0]); i++)
		rc = orm_status(
			db, norm_schema_create(scope.connection, models[i], &options, NULL, &error),
			&error);

	/* Historical manifest upgrades for databases predating file metadata and conflict
	 * resolution. */
	for (size_t i = 0; rc == 0 && i < NB_ARRAY_SIZE(columns); i++) {
		bool present = false;

		rc = orm_table_has_column(&scope, "main", columns[i].model->table.ptr,
					  columns[i].column, &present);
		if (rc == 0 && !present) {
			const struct norm_model *model = columns[i].model;
			size_t field = 0;

			while (field < model->field_count &&
			       !nb_str_eq_cstr(model->fields[field].column, columns[i].column))
				field++;

			if (field == model->field_count)
				return NB_ERR_STATE;

			rc = orm_status(db,
					norm_schema_add_column(scope.connection, model, field, NULL,
							       &error),
					&error);
		}
	}

	return rc;
}

int manifest_open(struct db **out, const char *sync_dir)
{
	nb_buf path NB_CLEANUP(nb_buf_destroy) = {0};

	if (!out || *out || !sync_dir)
		return NB_ERR_INVAL;

	int rc = nb_path_concat(&path, sync_dir, ".morph-sync");

	if (rc == 0)
		rc = nb_file_mkdir(nb_buf_cstr(&path), true, NULL);

	if (rc == 0)
		rc = nb_path_join(&path, nb_buf_cstr(&path), "manifest.db");

	if (rc != 0)
		return rc;

	struct db *db = calloc(1, sizeof(*db));

	if (!db)
		return NB_ERR_NOMEM;

	rc = db_open(NULL, db, nb_buf_cstr(&path));
	if (rc == 0)
		rc = manifest_initialize(db);

	if (rc != 0) {
		db_close(db);
		free(db);
		return rc;
	}

	*out = db;
	return 0;
}

void manifest_close(struct db **owner)
{
	if (!owner || !*owner)
		return;

	db_close(*owner);
	free(*owner);
	*owner = NULL;
}

int manifest_get(struct db *db, const char *path, struct manifest_entry *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	int rc = orm_scope_begin(db, &scope);
	const struct norm_select_query query = {
		.where = norm_eq(norm_col(&orm_sync_entries, 0), orm_string(path)),
	};

	memset(out, 0, sizeof(*out));
	if (rc == 0)
		rc = orm_status(db,
				norm_select(scope.connection, &orm_sync_entries, &query, NULL,
					    &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	const struct orm_sync_entries_row *row = norm_rows_at(rows, 0);

	if (!row)
		return 0;

	out->found = 1;
	if (!row->missing_local_hash)
		rc = nb_str_copy_to(row->local_hash, out->local_hash, sizeof(out->local_hash));

	if (rc == 0 && !row->missing_remote_hash)
		rc = nb_str_copy_to(row->remote_hash, out->remote_hash, sizeof(out->remote_hash));

	out->local_exists = (int)row->local_exists;
	out->remote_exists = (int)row->remote_exists;
	out->tombstone = (int)row->tombstone;
	out->local_size = row->local_size;
	out->remote_size = row->remote_size;
	out->local_mtime = row->local_mtime;
	out->remote_mtime = row->remote_mtime;
	return rc;
}

static void manifest_device_id(char *buffer, size_t capacity)
{
	if (gethostname(buffer, capacity) != 0 || !buffer[0]) {
		int written = snprintf(buffer, capacity, "device-%u", (unsigned)getpid());

		if (written < 0 || (size_t)written >= capacity)
			buffer[0] = '\0';
	}

	buffer[capacity - 1] = '\0';
}

int manifest_put(struct db *db, const struct sync_pair *pair, int tombstone)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	char device[128] = {0};
	int rc = orm_scope_begin(db, &scope);

	manifest_device_id(device, sizeof(device));
	const struct orm_sync_entries_row row = {
		.path = nb_str_from_cstr(pair->path),
		.local_hash = nb_str_from_cstr(pair->local.exists ? pair->local.hash : ""),
		.remote_hash = nb_str_from_cstr(pair->remote.exists ? pair->remote.hash : ""),
		.local_exists = pair->local.exists,
		.remote_exists = pair->remote.exists,
		.local_size = pair->local.exists ? pair->local.size : 0,
		.remote_size = pair->remote.exists ? pair->remote.size : 0,
		.local_mtime = pair->local.exists ? pair->local.mtime : 0,
		.remote_mtime = pair->remote.exists ? pair->remote.mtime : 0,
		.tombstone = tombstone,
		.last_device = nb_str_from_cstr(device),
		.updated_at = (int64_t)time(NULL),
	};
	struct norm_term values[11] = {0};
	struct norm_assignment set[11] = {0};
	struct norm_error error = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(set); i++) {
		values[i] = norm_term_excluded(norm_col(&orm_sync_entries, i + 1));
		set[i] = (struct norm_assignment){.column = norm_col(&orm_sync_entries, i + 1),
						  .expression = &values[i]};
	}

	const size_t keys[] = {0};
	const struct norm_conflict conflict = {
		.action = NORM_CONFLICT_UPDATE,
		.fields = keys,
		.field_count = 1,
		.set = set,
		.set_count = NB_ARRAY_SIZE(set),
	};
	const struct norm_insert_options options = {.conflict = &conflict};

	return rc != 0 ? rc
		       : orm_status(db,
				    norm_insert(scope.connection, &orm_sync_entries, &row, &options,
						NULL, NULL, &error),
				    &error);
}

int manifest_conflict(struct db *db, const struct sync_pair *pair)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	int rc = orm_scope_begin(db, &scope);
	const struct orm_sync_conflicts_row row = {
		.path = nb_str_from_cstr(pair->path),
		.local_hash = nb_str_from_cstr(pair->local.hash),
		.remote_hash = nb_str_from_cstr(pair->remote.hash),
		.created_at = (int64_t)time(NULL),
	};

	return rc != 0 ? rc
		       : orm_status(db,
				    norm_insert(scope.connection, &orm_sync_conflicts, &row, NULL,
						NULL, NULL, &error),
				    &error);
}

int manifest_resolve_db_conflicts(struct db *db, const char *path)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);
	struct norm_error error = {0};
	const struct norm_assignment set[] = {
		{.column = norm_col(&orm_sync_conflicts, 5),
		 .value = norm_i64((int64_t)time(NULL))},
	};
	const struct norm_expr predicates[] = {
		norm_eq(norm_col(&orm_sync_conflicts, 1), orm_string(path)),
		norm_eq(norm_col(&orm_sync_conflicts, 5), norm_i64(0)),
	};
	const struct norm_update_query query = {
		.set = set,
		.set_count = 1,
		.where = norm_and(predicates, 2),
	};

	return rc != 0 ? rc
		       : orm_status(db,
				    norm_update(scope.connection, &orm_sync_conflicts, &query, NULL,
						NULL, &error),
				    &error);
}

int manifest_trash(struct db *db, const char *path, const char *trash_path)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	int rc = orm_scope_begin(db, &scope);
	const struct orm_sync_trash_row row = {
		.path = nb_str_from_cstr(path),
		.trash_path = nb_str_from_cstr(trash_path),
		.created_at = (int64_t)time(NULL),
	};

	return rc != 0 ? rc
		       : orm_status(db,
				    norm_insert(scope.connection, &orm_sync_trash, &row, NULL, NULL,
						NULL, &error),
				    &error);
}

int manifest_tombstone(struct db *db, const char *path)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	char device[128] = {0};
	int rc = orm_scope_begin(db, &scope);

	manifest_device_id(device, sizeof(device));
	const struct orm_sync_tombstones_row row = {
		.path = nb_str_from_cstr(path),
		.deleted_at = (int64_t)time(NULL),
		.device_id = nb_str_from_cstr(device),
	};
	const struct norm_term deleted = norm_term_excluded(norm_col(&orm_sync_tombstones, 1));
	const struct norm_term device_term = norm_term_excluded(norm_col(&orm_sync_tombstones, 2));
	const struct norm_assignment set[] = {
		{.column = norm_col(&orm_sync_tombstones, 1), .expression = &deleted},
		{.column = norm_col(&orm_sync_tombstones, 2), .expression = &device_term},
	};
	const size_t keys[] = {0};
	const struct norm_conflict conflict = {
		.action = NORM_CONFLICT_UPDATE,
		.fields = keys,
		.field_count = 1,
		.set = set,
		.set_count = 2,
	};
	const struct norm_insert_options options = {.conflict = &conflict};
	struct norm_error error = {0};

	return rc != 0 ? rc
		       : orm_status(db,
				    norm_insert(scope.connection, &orm_sync_tombstones, &row,
						&options, NULL, NULL, &error),
				    &error);
}

int manifest_clear_tombstone(struct db *db, const char *path)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);
	struct norm_error error = {0};
	const struct norm_delete_query query = {
		.where = norm_eq(norm_col(&orm_sync_tombstones, 0), orm_string(path)),
	};

	return rc != 0 ? rc
		       : orm_status(db,
				    norm_delete(scope.connection, &orm_sync_tombstones, &query,
						NULL, NULL, &error),
				    &error);
}

int manifest_delete_trash(struct db *db, int64_t id)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);
	struct norm_error error = {0};
	const struct norm_delete_query query = {
		.where = norm_eq(norm_col(&orm_sync_trash, 0), norm_i64(id)),
	};

	return rc != 0 ? rc
		       : orm_status(db,
				    norm_delete(scope.connection, &orm_sync_trash, &query, NULL,
						NULL, &error),
				    &error);
}

static int manifest_count(struct db *db, const struct norm_model *model, bool unresolved)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const struct norm_table_ref source = norm_table(model, NB_STR_LIT("r"));
	const struct norm_query_field fields[] = {
		{.value = norm_term_call(NORM_FUNCTION_COUNT, NULL, 0), .into = 0},
	};
	const struct norm_query query = {
		.from = &source,
		.projection = &orm_integer_model,
		.fields = fields,
		.field_count = 1,
		.where = unresolved ? norm_eq(norm_table_col(&source, 5), norm_i64(0))
				    : (struct norm_expr){0},
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(db,
				norm_query_select(scope.connection, &query, NULL, &rows, &error),
				&error);

	const struct orm_integer *row = rc == 0 ? norm_rows_at(rows, 0) : NULL;

	return row ? (int)row->value : 0;
}

int manifest_trash_count(struct db *db)
{
	return manifest_count(db, &orm_sync_trash, false);
}

int manifest_unresolved_count(struct db *db)
{
	return manifest_count(db, &orm_sync_conflicts, true);
}

void manifest_trash_record_cleanup(struct manifest_trash_record *record)
{
	free(record->path);
	free(record->trash_path);
	memset(record, 0, sizeof(*record));
}

void manifest_trash_records_cleanup(struct manifest_trash_records *records)
{
	for (size_t i = 0; i < records->count; i++)
		manifest_trash_record_cleanup(&records->items[i]);

	free(records->items);
	memset(records, 0, sizeof(*records));
}

static int manifest_trash_copy_row(const struct orm_sync_trash_row *row,
				   struct manifest_trash_record *out)
{
	out->id = row->id;
	out->path = orm_text_copy(row->path);
	out->trash_path = orm_text_copy(row->trash_path);
	return out->path && out->trash_path ? 0 : NB_ERR_NOMEM;
}

int manifest_trash_expired_copy(struct db *db, int64_t cutoff, struct manifest_trash_records *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct manifest_trash_records records NB_CLEANUP(manifest_trash_records_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_result_options budget = {
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
		.max_row_bytes = SIZE_MAX,
	};
	const struct norm_select_query query = {
		.where = norm_lt(norm_col(&orm_sync_trash, 3), norm_i64(cutoff)),
		.result_options = &budget,
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(
			db,
			norm_select(scope.connection, &orm_sync_trash, &query, NULL, &rows, &error),
			&error);

	if (rc != 0)
		return rc;

	size_t count = norm_rows_count(rows);

	if (count) {
		records.items = calloc(count, sizeof(*records.items));
		if (!records.items)
			return NB_ERR_NOMEM;
	}

	for (size_t i = 0; rc == 0 && i < count; i++) {
		records.count++;
		rc = manifest_trash_copy_row(norm_rows_at(rows, i), &records.items[i]);
	}

	if (rc == 0) {
		*out = records;
		memset(&records, 0, sizeof(records));
	}

	return rc;
}

int manifest_trash_get_copy(struct db *db, int64_t id, struct manifest_trash_record *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct manifest_trash_record record NB_CLEANUP(manifest_trash_record_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_select_query query = {
		.where = norm_eq(norm_col(&orm_sync_trash, 0), norm_i64(id)),
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(
			db,
			norm_select(scope.connection, &orm_sync_trash, &query, NULL, &rows, &error),
			&error);

	if (rc != 0)
		return rc;

	const struct orm_sync_trash_row *row = norm_rows_at(rows, 0);

	if (!row)
		return NB_ERR_NOT_FOUND;

	rc = manifest_trash_copy_row(row, &record);
	if (rc == 0) {
		*out = record;
		memset(&record, 0, sizeof(record));
	}

	return rc;
}

int manifest_conflicts_copy(const char *path, struct runtime_sync_conflict **out, int *count)
{
	struct db database NB_CLEANUP(db_close) = {0};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	bool resolved = false;
	int rc = db_open_readonly(NULL, &database, path);

	if (rc != 0)
		return NB_ERR_NOT_FOUND;

	rc = orm_scope_begin(&database, &scope);
	if (rc == 0)
		rc = orm_table_has_column(&scope, "main", "conflicts", "resolved_at", &resolved);

	struct norm_error error = {0};
	const size_t fields[] = {0, 1, 4};
	const struct norm_order order = {.column = norm_col(&orm_sync_conflicts, 0),
					 .direction = NORM_DESC};
	const struct norm_select_query query = {
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.where = resolved ? norm_eq(norm_col(&orm_sync_conflicts, 5), norm_i64(0))
				  : (struct norm_expr){0},
		.order = &order,
		.order_count = 1,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};

	if (rc == 0)
		rc = orm_status(&database,
				norm_select(scope.connection, &orm_sync_conflicts, &query, NULL,
					    &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	size_t size = norm_rows_count(rows);

	if (size > INT_MAX || size > SIZE_MAX / sizeof(**out))
		return NB_ERR_RANGE;

	struct runtime_sync_conflict *items = size ? calloc(size, sizeof(*items)) : NULL;

	if (size && !items)
		return NB_ERR_NOMEM;

	for (size_t i = 0; rc == 0 && i < size; i++) {
		const struct orm_sync_conflicts_row *row = norm_rows_at(rows, i);

		items[i].id = row->id;
		items[i].created_at = row->created_at;
		rc = nb_str_copy_to(row->path, items[i].path, sizeof(items[i].path));
	}

	if (rc != 0) {
		free(items);
		return rc;
	}

	*out = items;
	*count = (int)size;
	return 0;
}
