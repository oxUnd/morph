#include "runtime_store.h"

#include "records.h"
#include "session_models.h"
#include <norm/query.h>
#include "errors.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/vec.h>
#include <stdlib.h>
#include <string.h>

struct runtime_instance_row {
	nb_str epoch;
	bool epoch_missing;
	int64_t pid;
	bool pid_missing;
	int32_t closed;
	bool closed_missing;
};
static const struct norm_field runtime_instance_fields[] = {
	NORM_TEXT_FIELD(struct runtime_instance_row, epoch, SIZE_MAX, NORM_FIELD_PRIMARY_KEY),
	NORM_FIELD(struct runtime_instance_row, pid, NORM_TYPE_I64, 0),
	{.column = NB_STR_LIT("closed"),
	 .type = NORM_TYPE_I32,
	 .offset = offsetof(struct runtime_instance_row, closed),
	 .size = sizeof(((struct runtime_instance_row *)0)->closed),
	 .null_offset = SIZE_MAX,
	 .default_sql = NB_STR_LIT("0")},
};
static const struct norm_model runtime_instance_model =
	NORM_MODEL(struct runtime_instance_row, "runtime_instances", runtime_instance_fields);
static const struct norm_field runtime_pending_fields[] = {
	NORM_NULLABLE_FIELD(struct runtime_instance_row, epoch, epoch_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct runtime_instance_row, pid, pid_missing, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct runtime_instance_row, closed, closed_missing, NORM_TYPE_I32, 0),
};
static const struct norm_model runtime_pending_model =
	NORM_MODEL(struct runtime_instance_row, "", runtime_pending_fields);

struct runtime_settings_row {
	int32_t access;
	bool access_missing;
	int32_t approval;
	bool approval_missing;
	int32_t reviewer;
	bool reviewer_missing;
	int32_t ultra;
	bool ultra_missing;
	int64_t session_id;
};
static const struct norm_field runtime_settings_fields[] = {
	NORM_NULLABLE_FIELD(struct runtime_settings_row, access, access_missing, NORM_TYPE_I32, 0),
	NORM_NULLABLE_FIELD(struct runtime_settings_row, approval, approval_missing, NORM_TYPE_I32,
			    0),
	NORM_NULLABLE_FIELD(struct runtime_settings_row, reviewer, reviewer_missing, NORM_TYPE_I32,
			    0),
	NORM_NULLABLE_FIELD(struct runtime_settings_row, ultra, ultra_missing, NORM_TYPE_I32, 0),
	NORM_FIELD(struct runtime_settings_row, session_id, NORM_TYPE_I64, NORM_FIELD_PRIMARY_KEY),
};
static const size_t runtime_session_id = SESSION_ID;
static const size_t runtime_settings_session_id = 4;
static const struct norm_foreign_key runtime_settings_foreign_keys[] = {
	{.name = NB_STR_LIT("runtime_settings_session_fk"),
	 .fields = &runtime_settings_session_id,
	 .field_count = 1,
	 .references = &session_model,
	 .reference_fields = &runtime_session_id,
	 .on_delete = NORM_FK_CASCADE},
};
static const struct norm_model runtime_settings_model =
	NORM_MODEL_WITH(struct runtime_settings_row, "runtime_session_settings",
			runtime_settings_fields, NORM_FOREIGN_KEYS(runtime_settings_foreign_keys));

struct runtime_turn_row {
	nb_str turn_id;
	bool turn_id_missing;
	nb_str status;
	bool status_missing;
	nb_str final_text;
	bool final_text_missing;
	int32_t execution_rc;
	int32_t persistence_rc;
	int64_t session_id;
	nb_str owner_epoch;
	nb_str user_id;
	nb_str idempotency_key;
	bool idempotency_key_missing;
	nb_str input;
	nb_str stored_input;
	int64_t updated_at;
};
static const struct norm_field runtime_turn_fields[] = {
	NORM_TEXT_FIELD(struct runtime_turn_row, turn_id, SIZE_MAX, NORM_FIELD_PRIMARY_KEY),
	NORM_TEXT_FIELD(struct runtime_turn_row, status, 0, 0),
	NORM_TEXT_FIELD(struct runtime_turn_row, final_text, 0, 0),
	{.column = NB_STR_LIT("execution_rc"),
	 .type = NORM_TYPE_I32,
	 .offset = offsetof(struct runtime_turn_row, execution_rc),
	 .size = sizeof(((struct runtime_turn_row *)0)->execution_rc),
	 .null_offset = SIZE_MAX,
	 .default_sql = NB_STR_LIT("0")},
	{.column = NB_STR_LIT("persistence_rc"),
	 .type = NORM_TYPE_I32,
	 .offset = offsetof(struct runtime_turn_row, persistence_rc),
	 .size = sizeof(((struct runtime_turn_row *)0)->persistence_rc),
	 .null_offset = SIZE_MAX,
	 .default_sql = NB_STR_LIT("0")},
	NORM_FIELD(struct runtime_turn_row, session_id, NORM_TYPE_I64, 0),
	NORM_TEXT_FIELD(struct runtime_turn_row, owner_epoch, 0, 0),
	NORM_TEXT_FIELD(struct runtime_turn_row, user_id, SIZE_MAX, 0),
	NORM_NULLABLE_TEXT_FIELD(struct runtime_turn_row, idempotency_key, idempotency_key_missing,
				 SIZE_MAX, 0),
	NORM_TEXT_FIELD(struct runtime_turn_row, input, 0, 0),
	NORM_TEXT_FIELD(struct runtime_turn_row, stored_input, 0, 0),
	{.column = NB_STR_LIT("updated_at"),
	 .type = NORM_TYPE_I64,
	 .offset = offsetof(struct runtime_turn_row, updated_at),
	 .size = sizeof(((struct runtime_turn_row *)0)->updated_at),
	 .null_offset = SIZE_MAX,
	 .default_sql = NB_STR_LIT("(strftime('%s','now'))")},
};
static const struct norm_model runtime_turn_model;
static const struct norm_expr runtime_turn_has_key = {
	.kind = NORM_EXPR_IS_NOT_NULL,
	.as.null_column = {.model = &runtime_turn_model, .index = 8},
};
static const struct norm_index runtime_turn_indexes[] = {
	{NORM_INDEX_METADATA("runtime_turns_key", true, 5, 7, 8),
	 .predicate = &runtime_turn_has_key},
	NORM_INDEX("runtime_turns_session", 5, 11),
};
static const size_t runtime_turn_session_id = 5;
static const struct norm_foreign_key runtime_turn_foreign_keys[] = {
	{.name = NB_STR_LIT("runtime_turn_session_fk"),
	 .fields = &runtime_turn_session_id,
	 .field_count = 1,
	 .references = &session_model,
	 .reference_fields = &runtime_session_id,
	 .on_delete = NORM_FK_CASCADE},
};
static const struct norm_model runtime_turn_model = NORM_MODEL_WITH(
	struct runtime_turn_row, "runtime_turns", runtime_turn_fields,
	NORM_FOREIGN_KEYS(runtime_turn_foreign_keys), NORM_INDEXES(runtime_turn_indexes));

struct runtime_store_instance_buffer {
	nb_vec entries;
};

static void runtime_store_instance_buffer_cleanup(struct runtime_store_instance_buffer *buffer)
{
	for (size_t i = 0; i < buffer->entries.len; i++) {
		struct runtime_store_instance *item = nb_vec_at(&buffer->entries, i);

		free(item->epoch);
	}

	nb_vec_destroy(&buffer->entries);
}

void runtime_store_instances_cleanup(struct runtime_store_instances *instances)
{
	if (!instances)
		return;

	for (size_t i = 0; i < instances->count; i++)
		free(instances->items[i].epoch);

	free(instances->items);
	memset(instances, 0, sizeof(*instances));
}

int runtime_store_init(struct db *db)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_schema_options options = {.if_not_exists = true};
	const struct norm_model *const models[] = {&runtime_turn_model, &runtime_instance_model,
						   &runtime_settings_model};
	int rc = orm_scope_begin(db, &scope);

	for (size_t i = 0; rc == 0 && i < NB_ARRAY_SIZE(models); i++)
		rc = orm_status(
			db, norm_schema_create(scope.connection, models[i], &options, NULL, &error),
			&error);

	return rc;
}

int runtime_store_pending_instances_copy(struct db *db, struct runtime_store_instances *out)
{
	struct runtime_store_instance_buffer buffer NB_CLEANUP(
		runtime_store_instance_buffer_cleanup) = {0};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!out)
		return NB_ERR_INVAL;

	if (out->items || out->count)
		return NB_ERR_EXISTS;

	struct norm_error error = {0};
	const struct norm_table_ref turns = norm_table(&runtime_turn_model, NB_STR_LIT("t"));
	const struct norm_table_ref instances =
		norm_table(&runtime_instance_model, NB_STR_LIT("i"));
	const struct norm_join join = {
		.kind = NORM_JOIN_LEFT,
		.table = &instances,
		.on = norm_columns_eq(norm_table_col(&instances, 0), norm_table_col(&turns, 6))};
	const struct norm_query_field selected[] = {
		{norm_term_column(norm_table_col(&turns, 6)), 0},
		{norm_term_column(norm_table_col(&instances, 1)), 1},
		{norm_term_column(norm_table_col(&instances, 2)), 2},
	};
	const struct norm_value states[] = {
		orm_string("queued"),	  orm_string("running"),   orm_string("preparing"),
		orm_string("cancelling"), orm_string("finishing"),
	};
	const struct norm_result_options limits = {
		.max_rows = SIZE_MAX, .max_bytes = SIZE_MAX, .max_row_bytes = SIZE_MAX};
	const struct norm_query query = {
		.from = &turns,
		.joins = &join,
		.join_count = 1,
		.projection = &runtime_pending_model,
		.fields = selected,
		.field_count = NB_ARRAY_SIZE(selected),
		.where = norm_in(norm_table_col(&turns, 1), states, NB_ARRAY_SIZE(states)),
		.distinct = true,
		.result_options = &limits};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(db,
				norm_query_select(scope.connection, &query, NULL, &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	nb_vec_init(&buffer.entries, sizeof(struct runtime_store_instance));
	for (size_t i = 0; i < norm_rows_count(rows); i++) {
		const struct runtime_instance_row *row = norm_rows_at(rows, i);
		struct runtime_store_instance *slot = nb_vec_push(&buffer.entries);

		if (!slot)
			return NB_ERR_NOMEM;

		memset(slot, 0, sizeof(*slot));
		slot->epoch = row->epoch_missing ? strdup("") : orm_text_copy(row->epoch);
		if (!slot->epoch)
			return NB_ERR_NOMEM;

		slot->process_id = row->pid;
		slot->missing = row->pid_missing;
		slot->closed = row->closed;
	}

	if (buffer.entries.len) {
		out->items = calloc(buffer.entries.len, sizeof(*out->items));
		if (!out->items)
			return NB_ERR_NOMEM;
	}

	out->count = buffer.entries.len;
	for (size_t i = 0; i < out->count; i++) {
		struct runtime_store_instance *source = nb_vec_at(&buffer.entries, i);

		out->items[i].epoch = nb_charp_steal(&source->epoch);
		out->items[i].process_id = source->process_id;
		out->items[i].missing = source->missing;
		out->items[i].closed = source->closed;
	}

	return 0;
}

int runtime_store_instance_register(struct db *db, const char *epoch, int64_t process_id)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};

	if (!epoch || !epoch[0] || process_id <= 0)
		return NB_ERR_INVAL;

	const struct runtime_instance_row row = {.epoch = nb_str_from_cstr(epoch),
						 .pid = process_id};
	const size_t key = 0;
	const struct norm_term excluded[] = {
		norm_term_excluded(norm_col(&runtime_instance_model, 1)),
		norm_term_excluded(norm_col(&runtime_instance_model, 2)),
	};
	const struct norm_assignment changes[] = {
		{.column = norm_col(&runtime_instance_model, 1), .expression = &excluded[0]},
		{.column = norm_col(&runtime_instance_model, 2), .expression = &excluded[1]},
	};
	const struct norm_conflict conflict = {.action = NORM_CONFLICT_UPDATE,
					       .fields = &key,
					       .field_count = 1,
					       .set = changes,
					       .set_count = NB_ARRAY_SIZE(changes)};
	const struct norm_insert_options options = {.conflict = &conflict};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_insert(scope.connection, &runtime_instance_model, &row,
					   &options, NULL, NULL, &error),
			       &error);
}

int runtime_store_instance_close(struct db *db, const char *epoch)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};

	if (!epoch || !epoch[0])
		return NB_ERR_INVAL;

	const struct norm_assignment change = {.column = norm_col(&runtime_instance_model, 2),
					       .value = norm_i32(1)};
	const struct norm_update_query query = {
		.set = &change,
		.set_count = 1,
		.where = norm_eq(norm_col(&runtime_instance_model, 0), orm_string(epoch))};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_update(scope.connection, &runtime_instance_model, &query, NULL,
					   NULL, &error),
			       &error);
}

int runtime_store_recover_epoch(struct db *db, const char *epoch, int execution_rc)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};

	if (!epoch || !epoch[0])
		return NB_ERR_INVAL;

	const struct norm_term now = norm_term_call(NORM_FUNCTION_CURRENT_EPOCH, NULL, 0);
	const struct norm_assignment changes[] = {
		{.column = norm_col(&runtime_turn_model, 1), .value = orm_string("interrupted")},
		{.column = norm_col(&runtime_turn_model, 3), .value = norm_i32(execution_rc)},
		{.column = norm_col(&runtime_turn_model, 11), .expression = &now},
	};
	const struct norm_value states[] = {
		orm_string("queued"),	  orm_string("running"),   orm_string("preparing"),
		orm_string("cancelling"), orm_string("finishing"),
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&runtime_turn_model, 6), orm_string(epoch)),
		norm_in(norm_col(&runtime_turn_model, 1), states, NB_ARRAY_SIZE(states)),
	};
	const struct norm_update_query query = {
		.set = changes,
		.set_count = NB_ARRAY_SIZE(changes),
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions))};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_update(scope.connection, &runtime_turn_model, &query, NULL,
					   NULL, &error),
			       &error);
}

int runtime_store_session_settings_get(struct db *db, int64_t session_id,
				       struct runtime_store_session_settings *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};

	if (!out || session_id <= 0)
		return NB_ERR_INVAL;

	const size_t fields[] = {0, 1, 2, 3};
	const struct norm_select_query query = {
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.where = norm_eq(norm_col(&runtime_settings_model, 4), norm_i64(session_id)),
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(db,
				norm_select(scope.connection, &runtime_settings_model, &query, NULL,
					    &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return NB_ERR_NO_DATA;

	const struct runtime_settings_row *row = norm_rows_at(rows, 0);

	*out = (struct runtime_store_session_settings){
		.session_id = session_id,
		.has_permission = !row->access_missing,
		.access = row->access,
		.approval = row->approval,
		.reviewer = row->reviewer,
		.has_ultra = !row->ultra_missing,
		.ultra = row->ultra,
	};
	return 0;
}

int runtime_store_session_settings_put(struct db *db,
				       const struct runtime_store_session_settings *settings)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};

	if (!settings || settings->session_id <= 0)
		return NB_ERR_INVAL;

	const struct runtime_settings_row row = {
		.session_id = settings->session_id,
		.access = settings->access,
		.access_missing = !settings->has_permission,
		.approval = settings->approval,
		.approval_missing = !settings->has_permission,
		.reviewer = settings->reviewer,
		.reviewer_missing = !settings->has_permission,
		.ultra = settings->ultra,
		.ultra_missing = !settings->has_ultra,
	};
	const size_t key = 4;
	struct norm_term excluded[4] = {0};
	struct norm_assignment changes[4] = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(changes); i++) {
		excluded[i] = norm_term_excluded(norm_col(&runtime_settings_model, i));
		changes[i] = (struct norm_assignment){
			.column = norm_col(&runtime_settings_model, i), .expression = &excluded[i]};
	}

	const struct norm_conflict conflict = {.action = NORM_CONFLICT_UPDATE,
					       .fields = &key,
					       .field_count = 1,
					       .set = changes,
					       .set_count = NB_ARRAY_SIZE(changes)};
	const struct norm_insert_options options = {.conflict = &conflict};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_insert(scope.connection, &runtime_settings_model, &row,
					   &options, NULL, NULL, &error),
			       &error);
}

int runtime_store_turn_put(struct db *db, const struct runtime_store_turn_input *turn)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};

	if (!turn || !turn->turn_id || !turn->turn_id[0] || turn->session_id <= 0 ||
	    !turn->owner_epoch || !turn->owner_epoch[0] || !turn->user_id || !turn->input ||
	    !turn->stored_input || !turn->status)
		return NB_ERR_INVAL;

	const struct norm_query_field selected[] = {
		{norm_term_value(orm_string(turn->turn_id)), 0},
		{norm_term_value(orm_string(turn->status)), 1},
		{norm_term_value(orm_string(turn->final_text ? turn->final_text : "")), 2},
		{norm_term_value(norm_i32(turn->execution_rc)), 3},
		{norm_term_value(norm_i32(turn->persistence_rc)), 4},
		{norm_term_value(norm_i64(turn->session_id)), 5},
		{norm_term_value(orm_string(turn->owner_epoch)), 6},
		{norm_term_value(orm_string(turn->user_id)), 7},
		{norm_term_value(turn->idempotency_key && turn->idempotency_key[0]
					 ? orm_string(turn->idempotency_key)
					 : norm_null()),
		 8},
		{norm_term_value(orm_string(turn->input)), 9},
		{norm_term_value(orm_string(turn->stored_input)), 10},
		{norm_term_call(NORM_FUNCTION_CURRENT_EPOCH, NULL, 0), 11},
	};
	const size_t fields[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
	const size_t key = 0;
	const size_t updated[] = {1, 2, 3, 4, 11};
	struct norm_term excluded[NB_ARRAY_SIZE(updated)] = {0};
	struct norm_assignment changes[NB_ARRAY_SIZE(updated)] = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(updated); i++) {
		excluded[i] = norm_term_excluded(norm_col(&runtime_turn_model, updated[i]));
		changes[i] = (struct norm_assignment){
			.column = norm_col(&runtime_turn_model, updated[i]),
			.expression = &excluded[i]};
	}

	const struct norm_conflict conflict = {.action = NORM_CONFLICT_UPDATE,
					       .fields = &key,
					       .field_count = 1,
					       .set = changes,
					       .set_count = NB_ARRAY_SIZE(changes)};
	const struct norm_query query = {.projection = &runtime_turn_model,
					 .fields = selected,
					 .field_count = NB_ARRAY_SIZE(selected)};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_insert_select(scope.connection, &runtime_turn_model, fields,
						  NB_ARRAY_SIZE(fields), &query, &conflict, NULL,
						  NULL, &error),
			       &error);
}

void runtime_store_turn_record_cleanup(struct runtime_store_turn_record *record)
{
	if (!record)
		return;

	free(record->turn_id);
	free(record->status);
	free(record->final_text);
	memset(record, 0, sizeof(*record));
}

static int runtime_store_turn_row_copy(const struct runtime_turn_row *row,
				       struct runtime_store_turn_record *out)
{
	struct runtime_store_turn_record value NB_CLEANUP(runtime_store_turn_record_cleanup) = {0};
	const char *id = row->turn_id_missing ? NULL : row->turn_id.ptr;
	const char *status = row->status_missing ? NULL : row->status.ptr;
	const char *text = row->final_text_missing ? NULL : row->final_text.ptr;

	value.turn_id = id ? orm_text_copy(row->turn_id) : strdup("");
	value.status = status ? orm_text_copy(row->status) : strdup("interrupted");
	value.final_text = text ? orm_text_copy(row->final_text) : strdup("");
	if (!value.turn_id || !value.status || !value.final_text)
		return NB_ERR_NOMEM;

	value.execution_rc = row->execution_rc;
	value.persistence_rc = row->persistence_rc;
	value.session_id = row->session_id;
	out->turn_id = nb_charp_steal(&value.turn_id);
	out->status = nb_charp_steal(&value.status);
	out->final_text = nb_charp_steal(&value.final_text);
	out->session_id = value.session_id;
	out->execution_rc = value.execution_rc;
	out->persistence_rc = value.persistence_rc;
	return 0;
}

static int runtime_store_turn_read(struct db *db, int64_t session_id, const char *turn_id,
				   const char *user_id, const char *key,
				   struct runtime_store_turn_record *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!out || session_id <= 0)
		return NB_ERR_INVAL;

	if (out->turn_id || out->status || out->final_text)
		return NB_ERR_EXISTS;

	int rc = orm_scope_begin(db, &scope);

	if (rc != 0)
		return rc;

	struct norm_error error = {0};
	const struct norm_table_ref turns = norm_table(&runtime_turn_model, NB_STR_LIT("t"));
	struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&turns, 5), norm_i64(session_id)),
		{0},
		{0},
	};
	size_t condition_count = 1;

	if (key) {
		conditions[condition_count++] =
			norm_eq(norm_table_col(&turns, 7), orm_string(user_id));
		conditions[condition_count++] = norm_eq(norm_table_col(&turns, 8), orm_string(key));
	} else if (turn_id && turn_id[0]) {
		conditions[condition_count++] =
			norm_eq(norm_table_col(&turns, 0), orm_string(turn_id));
	}

	bool latest = condition_count == 1;
	struct norm_query_field selected[6] = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(selected); i++)
		selected[i] =
			(struct norm_query_field){norm_term_column(norm_table_col(&turns, i)), i};

	const struct norm_query_order order = {.value = norm_term_rowid(&turns),
					       .direction = NORM_DESC};
	const struct norm_result_options limits = {
		.max_rows = SIZE_MAX, .max_bytes = SIZE_MAX, .max_row_bytes = SIZE_MAX};
	const struct norm_query query = {.from = &turns,
					 .projection = &runtime_turn_model,
					 .fields = selected,
					 .field_count = NB_ARRAY_SIZE(selected),
					 .where = norm_and(conditions, condition_count),
					 .order = latest ? &order : NULL,
					 .order_count = latest ? 1 : 0,
					 .has_limit = latest,
					 .limit = latest ? 1 : 0,
					 .result_options = &limits};

	rc = orm_status(db, norm_query_select(scope.connection, &query, NULL, &rows, &error),
			&error);
	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return NB_ERR_NOT_FOUND;

	return runtime_store_turn_row_copy(norm_rows_at(rows, 0), out);
}

int runtime_store_turn_copy(struct db *db, int64_t session_id, const char *turn_id,
			    struct runtime_store_turn_record *out)
{
	return runtime_store_turn_read(db, session_id, turn_id, NULL, NULL, out);
}

int runtime_store_turn_by_key_copy(struct db *db, int64_t session_id, const char *user_id,
				   const char *key, struct runtime_store_turn_record *out)
{
	if (!user_id || !key || !key[0])
		return NB_ERR_INVAL;

	return runtime_store_turn_read(db, session_id, NULL, user_id, key, out);
}
