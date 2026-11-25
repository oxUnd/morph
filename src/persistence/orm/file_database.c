#include <norm/query.h>
#include "persistence/orm/sync_display_models.h"
#include "persistence/orm/session_models.h"
#include "persistence/orm/records.h"
#include "persistence/orm/file_database.h"
#include "persistence/orm/internal.h"
#include <nb/file.h>
#include <stdio.h>
#include <string.h>

static void file_cleanup(FILE **owner)
{
	if (*owner)
		fclose(*owner);

	*owner = NULL;
}

static void backup_cleanup(struct norm_sqlite_backup **owner)
{
	struct norm_error error = {0};

	if (*owner)
		norm_sqlite_backup_close(owner, &error);
}

int orm_database_quick_check(const char *path)
{
	struct db database NB_CLEANUP(db_close) = {0};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	size_t issues = 0;
	int rc = db_open_readonly(NULL, &database, path);

	if (rc == 0)
		rc = orm_scope_begin(&database, &scope);

	if (rc == 0)
		rc = orm_status(&database,
				norm_sqlite_check(scope.connection, NORM_SQLITE_CHECK_QUICK, NULL,
						  NULL, NULL, &issues, &error),
				&error);

	return rc == 0 && issues == 0;
}

int orm_database_is_sqlite(const char *path)
{
	struct db database NB_CLEANUP(db_close) = {0};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_sqlite_info info = {0};
	struct norm_error error = {0};
	unsigned char header[16];

	if (!path)
		return 0;

	FILE *file NB_CLEANUP(file_cleanup) = fopen(path, "rb");

	if (!file)
		return 0;

	size_t count = fread(header, 1, sizeof(header), file);

	if (count != sizeof(header) || memcmp(header, "SQLite format 3\0", sizeof(header)) != 0)
		return 0;

	int rc = db_open_readonly(NULL, &database, path);

	if (rc == 0)
		rc = orm_scope_begin(&database, &scope);

	if (rc == 0)
		rc = orm_status(&database,
				norm_sqlite_get_info(scope.connection, NULL, &info, &error),
				&error);

	return rc == 0;
}

int orm_database_snapshot(const char *source, const char *destination, int *page_size)
{
	struct db input NB_CLEANUP(db_close) = {0};
	struct db output NB_CLEANUP(db_close) = {0};
	struct orm_scope from NB_CLEANUP(orm_scope_cleanup) = {0};
	struct orm_scope to NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_sqlite_backup *backup NB_CLEANUP(backup_cleanup) = NULL;
	struct norm_sqlite_backup_progress progress = {0};
	struct norm_sqlite_info info = {0};
	struct norm_error error = {0};
	int rc = db_open_readonly(NULL, &input, source);

	if (rc == 0)
		rc = db_open_plain(NULL, &output, destination);

	if (rc == 0)
		rc = orm_scope_begin(&input, &from);

	if (rc == 0)
		rc = orm_scope_begin(&output, &to);

	if (rc == 0)
		rc = orm_status(&output,
				norm_sqlite_backup_open(from.connection, to.connection, NULL,
							&backup, &error),
				&error);

	while (rc == 0 && !progress.done)
		rc = orm_status(&output,
				norm_sqlite_backup_step(backup, 256, 5000, NULL, &progress, &error),
				&error);

	if (rc == 0)
		rc = orm_status(&output, norm_sqlite_backup_close(&backup, &error), &error);

	if (rc == 0)
		rc = orm_status(&output,
				norm_sqlite_set_journal_mode(
					to.connection, NORM_SQLITE_JOURNAL_DELETE, NULL, &error),
				&error);

	if (rc == 0 && page_size) {
		rc = orm_status(&output, norm_sqlite_get_info(to.connection, NULL, &info, &error),
				&error);
		if (rc == 0)
			*page_size = info.page_size;
	}

	return rc;
}

struct snapshot_identity_row {
	int64_t session_id;
	bool session_id_missing;
	nb_str identity;
	bool identity_missing;
};
static const struct norm_field snapshot_identity_fields[] = {
	NORM_NULLABLE_FIELD(struct snapshot_identity_row, session_id, session_id_missing,
			    NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct snapshot_identity_row, identity, identity_missing,
			    NORM_TYPE_TEXT, 0),
};
static const struct norm_model snapshot_identity_model =
	NORM_MODEL(struct snapshot_identity_row, "ui_session_identity", snapshot_identity_fields);
struct snapshot_session_id {
	int64_t value;
	bool missing;
};
static const struct norm_field snapshot_session_id_fields[] = {
	NORM_NULLABLE_FIELD(struct snapshot_session_id, value, missing, NORM_TYPE_I64, 0),
};
static const struct norm_model snapshot_session_id_model = {
	.size = sizeof(struct snapshot_session_id),
	.alignment = _Alignof(struct snapshot_session_id),
	.fields = snapshot_session_id_fields,
	.field_count = 1,
};

static int snapshot_copy_display(struct orm_scope *scope, const struct norm_model *source_model,
				 const struct norm_model *target_model)
{
	const struct norm_table_ref source = norm_table(source_model, NB_STR_LIT("u"));
	struct norm_query_field fields[12] = {0};
	struct norm_error error = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(fields); i++)
		fields[i] = (struct norm_query_field){
			.value = norm_term_column(norm_table_col(&source, i)), .into = i};

	const struct norm_query query = {
		.from = &source,
		.projection = &sync_display_model,
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
	};

	return orm_status(scope->database,
			  norm_insert_select(scope->connection, target_model, NULL, 0, &query, NULL,
					     NULL, NULL, &error),
			  &error);
}

static int file_column_present(struct orm_scope *scope, const char *schema, const char *table,
			       const char *column)
{
	bool present = false;

	return orm_table_has_column(scope, schema, table, column, &present) == 0 && present;
}

int orm_database_merge_display(const char *snapshot, const char *display_snapshot)
{
	struct db database NB_CLEANUP(db_close) = {0};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	int rc = db_open_plain(NULL, &database, snapshot);

	if (rc == 0)
		rc = orm_scope_begin(&database, &scope);

	if (rc == 0)
		rc = orm_status(&database,
				norm_sqlite_attach(scope.connection,
						   nb_str_from_cstr(display_snapshot),
						   NB_STR_LIT("display"), NULL, &error),
				&error);

	if (rc != 0)
		return rc;

	struct norm_model display_model = sync_display_model;
	display_model.schema = NB_STR_LIT("display");
	display_model.table = NB_STR_LIT("ui_messages");
	struct norm_model local_model = sync_display_model;
	local_model.schema = NB_STR_LIT("temp");
	local_model.table = NB_STR_LIT("local_display");
	const struct norm_schema_options create = {.if_not_exists = true};

	rc = orm_transaction_begin(&scope, NORM_SQLITE_IMMEDIATE);
	if (rc == 0)
		rc = orm_status(
			&database,
			norm_schema_create(scope.connection, &local_model, &create, NULL, &error),
			&error);

	if (rc == 0)
		rc = orm_status(&database,
				norm_schema_create(scope.connection, &sync_display_model, &create,
						   NULL, &error),
				&error);

	if (rc == 0)
		rc = snapshot_copy_display(&scope, &display_model, &local_model);

	const struct norm_table_ref local = norm_table(&local_model, NB_STR_LIT("u"));
	if (rc == 0 && file_column_present(&scope, "display", "ui_session_identity", "identity") &&
	    file_column_present(&scope, "main", "sessions", "display_id")) {
		struct norm_model identity_model = snapshot_identity_model;
		identity_model.schema = NB_STR_LIT("display");
		const struct norm_table_ref identity = norm_table(&identity_model, NB_STR_LIT("i"));
		const struct norm_table_ref sessions = norm_table(&session_model, NB_STR_LIT("s"));
		const struct norm_join join = {
			.kind = NORM_JOIN_INNER,
			.table = &sessions,
			.on = norm_columns_eq(norm_table_col(&sessions, SESSION_ID),
					      norm_table_col(&identity, 0))};
		const struct norm_term saved = norm_term_column(norm_table_col(&identity, 1));
		const struct norm_term current =
			norm_term_column(norm_table_col(&sessions, SESSION_DISPLAY_ID));
		const struct norm_query_field fields[] = {
			{.value = norm_term_column(norm_table_col(&identity, 0)), .into = 0}};
		const struct norm_query mismatched = {
			.from = &identity,
			.joins = &join,
			.join_count = 1,
			.projection = &snapshot_session_id_model,
			.fields = fields,
			.field_count = 1,
			.where = norm_compare_terms(&saved, NORM_COMPARE_NE, &current),
		};
		const struct norm_term local_id = norm_term_column(norm_col(&local_model, 0));
		const struct norm_delete_query remove = {
			.where = norm_in_query(&local_id, &mismatched, false)};

		rc = orm_status(
			&database,
			norm_delete(scope.connection, &local_model, &remove, NULL, NULL, &error),
			&error);
	}

	const struct norm_query_field session_ids[] = {
		{.value = norm_term_column(norm_table_col(&local, 0)), .into = 0}};
	const struct norm_query changed = {
		.from = &local,
		.projection = &snapshot_session_id_model,
		.fields = session_ids,
		.field_count = 1,
		.distinct = true,
	};
	const struct norm_term target_id = norm_term_column(norm_col(&sync_display_model, 0));
	const struct norm_delete_query replace = {
		.where = norm_in_query(&target_id, &changed, false)};

	if (rc == 0)
		rc = orm_status(&database,
				norm_delete(scope.connection, &sync_display_model, &replace, NULL,
					    NULL, &error),
				&error);

	if (rc == 0)
		rc = snapshot_copy_display(&scope, &local_model, &sync_display_model);

	const size_t indexed[] = {0, 3};
	const struct norm_index index = {
		.name = NB_STR_LIT("sync_ui_session_seq"), .fields = indexed, .field_count = 2};
	if (rc == 0)
		rc = orm_status(&database,
				norm_index_create(scope.connection, &sync_display_model, &index,
						  true, NULL, &error),
				&error);

	if (rc == 0)
		rc = orm_transaction_commit(&scope);

	if (rc != 0)
		(void)orm_transaction_rollback(&scope);

	int detached = orm_status(
		&database,
		norm_sqlite_detach(scope.connection, NB_STR_LIT("display"), NULL, &error), &error);

	return rc != 0 ? rc : detached;
}
