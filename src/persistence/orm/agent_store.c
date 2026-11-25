#include "agent_store.h"
#include "business_store.h"
#include "records.h"
#include "history_models.h"
#include <norm/query.h>
#include "errors.h"
#include <stdlib.h>
#include <string.h>

struct agent_thread_row {
	int64_t root_session_id;
	nb_str agent_id;
	bool agent_id_missing;
	nb_str parent_agent_id;
	bool parent_agent_id_missing;
	int64_t session_id;
	nb_str model;
	bool model_missing;
	nb_str effort;
	bool effort_missing;
	int32_t depth;
	nb_str description;
	bool description_missing;
	int32_t status;
	nb_str result;
	bool result_missing;
	int32_t error_code;
	int32_t iterations;
	int64_t started_at;
	int64_t ended_at;
	nb_str assignment_json;
	bool assignment_json_missing;
};

static const struct norm_field agent_thread_fields[] = {
	NORM_FIELD(struct agent_thread_row, root_session_id, NORM_TYPE_I64, 0),
	NORM_NULLABLE_TEXT_FIELD(struct agent_thread_row, agent_id, agent_id_missing, 0, 0),
	NORM_NULLABLE_TEXT_FIELD(struct agent_thread_row, parent_agent_id, parent_agent_id_missing,
				 0, 0),
	NORM_FIELD(struct agent_thread_row, session_id, NORM_TYPE_I64, 0),
	NORM_NULLABLE_TEXT_FIELD(struct agent_thread_row, model, model_missing, 0, 0),
	NORM_NULLABLE_TEXT_FIELD(struct agent_thread_row, effort, effort_missing, 0, 0),
	NORM_FIELD(struct agent_thread_row, depth, NORM_TYPE_I32, 0),
	NORM_NULLABLE_TEXT_FIELD(struct agent_thread_row, description, description_missing, 0, 0),
	NORM_FIELD(struct agent_thread_row, status, NORM_TYPE_I32, 0),
	NORM_NULLABLE_TEXT_FIELD(struct agent_thread_row, result, result_missing, 0, 0),
	NORM_FIELD(struct agent_thread_row, error_code, NORM_TYPE_I32, 0),
	NORM_FIELD(struct agent_thread_row, iterations, NORM_TYPE_I32, 0),
	NORM_FIELD(struct agent_thread_row, started_at, NORM_TYPE_I64, 0),
	NORM_FIELD(struct agent_thread_row, ended_at, NORM_TYPE_I64, 0),
	NORM_NULLABLE_TEXT_FIELD(struct agent_thread_row, assignment_json, assignment_json_missing,
				 0, 0),
};

static const struct norm_model agent_thread_model =
	NORM_MODEL(struct agent_thread_row, "agent_threads", agent_thread_fields);

struct agent_budget_row {
	int64_t root_session_id;
	int64_t used;
	int64_t input_tokens;
	int64_t cached_tokens;
	int64_t output_tokens;
	int32_t reminder_level;
};

static const struct norm_field agent_budget_fields[] = {
	NORM_FIELD(struct agent_budget_row, used, NORM_TYPE_I64, 0),
	NORM_FIELD(struct agent_budget_row, input_tokens, NORM_TYPE_I64, 0),
	NORM_FIELD(struct agent_budget_row, cached_tokens, NORM_TYPE_I64, 0),
	NORM_FIELD(struct agent_budget_row, output_tokens, NORM_TYPE_I64, 0),
	NORM_FIELD(struct agent_budget_row, reminder_level, NORM_TYPE_I32, 0),
	NORM_FIELD(struct agent_budget_row, root_session_id, NORM_TYPE_I64, 0),
};

static const struct norm_model agent_budget_model =
	NORM_MODEL(struct agent_budget_row, "agent_budget_state", agent_budget_fields);

struct agent_mail_row {
	int64_t root_session_id;
	nb_str recipient;
	int32_t consumed;
	int64_t id;
	nb_str sender;
	bool sender_missing;
	nb_str kind;
	bool kind_missing;
	nb_str body;
	bool body_missing;
};

static const struct norm_field agent_mail_fields[] = {
	NORM_FIELD(struct agent_mail_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_NULLABLE_TEXT_FIELD(struct agent_mail_row, sender, sender_missing, 0, 0),
	NORM_NULLABLE_TEXT_FIELD(struct agent_mail_row, kind, kind_missing, 0, 0),
	NORM_NULLABLE_TEXT_FIELD(struct agent_mail_row, body, body_missing, 0, 0),
	NORM_FIELD(struct agent_mail_row, root_session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct agent_mail_row, recipient, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct agent_mail_row, consumed, NORM_TYPE_I32, 0),
};

static const struct norm_model agent_mail_model =
	NORM_MODEL(struct agent_mail_row, "agent_mailbox", agent_mail_fields);

struct agent_event_row {
	int64_t id;
	int64_t root_session_id;
	nb_str agent_id;
	nb_str event_json;
};

static const struct norm_field agent_event_fields[] = {
	NORM_FIELD(struct agent_event_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_FIELD(struct agent_event_row, root_session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct agent_event_row, agent_id, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct agent_event_row, event_json, NORM_TYPE_TEXT, 0),
};
static const struct norm_model agent_event_model =
	NORM_MODEL(struct agent_event_row, "agent_events", agent_event_fields);

void agent_store_threads_cleanup(nb_vec *records)
{
	for (size_t i = 0; i < records->len; i++) {
		struct store_agent_thread *record = nb_vec_at(records, i);

		free(record->agent_id);
		free(record->parent_agent_id);
		free(record->model);
		free(record->effort);
		free(record->description);
		free(record->result);
		free(record->assignment_json);
	}

	nb_vec_destroy(records);
}

void agent_store_mail_cleanup(nb_vec *records)
{
	for (size_t i = 0; i < records->len; i++) {
		struct store_agent_mail *record = nb_vec_at(records, i);

		free(record->sender);
		free(record->kind);
		free(record->body);
	}

	nb_vec_destroy(records);
}

int agent_store_thread_put(struct db *db, const struct store_agent_thread *record)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct agent_thread_row row = {
		.root_session_id = record->root_session_id,
		.agent_id = nb_str_from_cstr(record->agent_id),
		.agent_id_missing = !record->agent_id,
		.parent_agent_id = nb_str_from_cstr(record->parent_agent_id),
		.parent_agent_id_missing = !record->parent_agent_id,
		.session_id = record->session_id,
		.model = nb_str_from_cstr(record->model),
		.model_missing = !record->model,
		.effort = nb_str_from_cstr(record->effort),
		.effort_missing = !record->effort,
		.depth = record->depth,
		.description = nb_str_from_cstr(record->description),
		.description_missing = !record->description,
		.status = record->status,
		.result = nb_str_from_cstr(record->result),
		.result_missing = !record->result,
		.error_code = record->error_code,
		.iterations = record->iterations,
		.started_at = record->started_at,
		.ended_at = record->ended_at,
		.assignment_json = nb_str_from_cstr(record->assignment_json),
		.assignment_json_missing = !record->assignment_json,
	};
	const size_t keys[] = {0, 1};
	const size_t updated[] = {7, 8, 9, 10, 11, 12, 13, 14};
	struct norm_term excluded[NB_ARRAY_SIZE(updated)] = {0};
	struct norm_assignment changes[NB_ARRAY_SIZE(updated)] = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(updated); i++) {
		excluded[i] = norm_term_excluded(norm_col(&agent_thread_model, updated[i]));
		changes[i] = (struct norm_assignment){
			.column = norm_col(&agent_thread_model, updated[i]),
			.expression = &excluded[i]};
	}

	const struct norm_conflict conflict = {
		.action = NORM_CONFLICT_UPDATE,
		.fields = keys,
		.field_count = NB_ARRAY_SIZE(keys),
		.set = changes,
		.set_count = NB_ARRAY_SIZE(changes),
	};
	const struct norm_insert_options options = {.conflict = &conflict};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_insert(scope.connection, &agent_thread_model, &row, &options,
					   NULL, NULL, &error),
			       &error);
}

int agent_store_threads_copy(struct db *db, int64_t session_id, nb_vec *records)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const struct norm_table_ref threads = norm_table(&agent_thread_model, NB_STR_LIT("t"));
	struct norm_query_field fields[NB_ARRAY_SIZE(agent_thread_fields)] = {0};
	const struct norm_term id = norm_term_column(norm_table_col(&threads, 1));
	const struct norm_query_order order = {
		.value = norm_term_call(NORM_FUNCTION_LENGTH, &id, 1)};
	const struct norm_result_options limits = {
		.max_rows = SIZE_MAX, .max_bytes = SIZE_MAX, .max_row_bytes = SIZE_MAX};

	for (size_t i = 0; i < NB_ARRAY_SIZE(fields); i++)
		fields[i] =
			(struct norm_query_field){norm_term_column(norm_table_col(&threads, i)), i};

	const struct norm_query query = {
		.from = &threads,
		.projection = &agent_thread_model,
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.where = norm_eq(norm_table_col(&threads, 0), norm_i64(session_id)),
		.order = &order,
		.order_count = 1,
		.result_options = &limits,
	};
	int rc = orm_scope_begin(db, &scope);

	nb_vec_init(records, sizeof(struct store_agent_thread));
	if (rc == 0)
		rc = orm_status(db,
				norm_query_select(scope.connection, &query, NULL, &rows, &error),
				&error);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct agent_thread_row *row = norm_rows_at(rows, i);
		struct store_agent_thread *entry = nb_vec_push(records);

		if (!entry) {
			rc = NB_ERR_NOMEM;
			break;
		}

		memset(entry, 0, sizeof(*entry));
		entry->root_session_id = row->root_session_id;
		entry->session_id = row->session_id;
		entry->depth = row->depth;
		entry->status = row->status;
		entry->error_code = row->error_code;
		entry->iterations = row->iterations;
		entry->started_at = row->started_at;
		entry->ended_at = row->ended_at;
		if (rc == 0 && !row->agent_id_missing) {
			entry->agent_id = orm_text_copy(row->agent_id);
			if (!entry->agent_id)
				rc = NB_ERR_NOMEM;
		}

		if (rc == 0 && !row->parent_agent_id_missing) {
			entry->parent_agent_id = orm_text_copy(row->parent_agent_id);
			if (!entry->parent_agent_id)
				rc = NB_ERR_NOMEM;
		}

		if (rc == 0 && !row->model_missing) {
			entry->model = orm_text_copy(row->model);
			if (!entry->model)
				rc = NB_ERR_NOMEM;
		}

		if (rc == 0 && !row->effort_missing) {
			entry->effort = orm_text_copy(row->effort);
			if (!entry->effort)
				rc = NB_ERR_NOMEM;
		}

		if (rc == 0 && !row->description_missing) {
			entry->description = orm_text_copy(row->description);
			if (!entry->description)
				rc = NB_ERR_NOMEM;
		}

		if (rc == 0 && !row->result_missing) {
			entry->result = orm_text_copy(row->result);
			if (!entry->result)
				rc = NB_ERR_NOMEM;
		}

		if (rc == 0 && !row->assignment_json_missing) {
			entry->assignment_json = orm_text_copy(row->assignment_json);
			if (!entry->assignment_json)
				rc = NB_ERR_NOMEM;
		}
	}

	if (rc != 0)
		agent_store_threads_cleanup(records);

	return rc;
}

int agent_store_mail_send(struct db *db, int64_t root, const char *sender, const char *recipient,
			  const char *kind, const char *body)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct agent_mail_row row = {
		.root_session_id = root,
		.recipient = nb_str_from_cstr(recipient),
		.sender = nb_str_from_cstr(sender),
		.sender_missing = !sender,
		.kind = nb_str_from_cstr(kind),
		.kind_missing = !kind,
		.body = nb_str_from_cstr(body),
		.body_missing = !body,
	};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_insert(scope.connection, &agent_mail_model, &row, NULL, NULL,
					   NULL, &error),
			       &error);
}

int agent_store_finish_mail_clear(struct db *db, int64_t root, const char *recipient)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&agent_mail_model, 4), norm_i64(root)),
		norm_eq(norm_col(&agent_mail_model, 5), orm_string(recipient)),
		norm_eq(norm_col(&agent_mail_model, 2), orm_string("FINISH_REQUEST")),
		norm_eq(norm_col(&agent_mail_model, 6), norm_i32(0)),
	};
	const struct norm_assignment change = {.column = norm_col(&agent_mail_model, 6),
					       .value = norm_i32(1)};
	const struct norm_update_query query = {
		.set = &change,
		.set_count = 1,
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
	};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_update(scope.connection, &agent_mail_model, &query, NULL, NULL,
					   &error),
			       &error);
}

int agent_store_mail_pending(struct db *db, int64_t root, const char *recipient, int new_tasks_only)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const size_t field = 0;
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&agent_mail_model, 4), norm_i64(root)),
		norm_eq(norm_col(&agent_mail_model, 5), orm_string(recipient)),
		norm_eq(norm_col(&agent_mail_model, 6), norm_i32(0)),
		norm_eq(norm_col(&agent_mail_model, 2), orm_string("NEW_TASK")),
	};
	const struct norm_select_query query = {
		.fields = &field,
		.field_count = 1,
		.where = norm_and(conditions, new_tasks_only ? 4 : 3),
		.has_limit = true,
		.limit = 1,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(db,
				norm_select(scope.connection, &agent_mail_model, &query, NULL,
					    &rows, &error),
				&error);

	return rc ? rc : norm_rows_count(rows) ? 1 : 0;
}

int agent_store_mail_copy(struct db *db, int64_t root, const char *recipient, nb_vec *records)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const size_t fields[] = {0, 1, 2, 3};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&agent_mail_model, 4), norm_i64(root)),
		norm_eq(norm_col(&agent_mail_model, 5), orm_string(recipient)),
		norm_eq(norm_col(&agent_mail_model, 6), norm_i32(0)),
	};
	const struct norm_order order = {.column = norm_col(&agent_mail_model, 0)};
	const struct norm_select_query query = {
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
		.order = &order,
		.order_count = 1,
		.has_limit = true,
		.limit = 32,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};
	int rc = orm_scope_begin(db, &scope);

	nb_vec_init(records, sizeof(struct store_agent_mail));
	if (rc == 0)
		rc = orm_status(db,
				norm_select(scope.connection, &agent_mail_model, &query, NULL,
					    &rows, &error),
				&error);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct agent_mail_row *row = norm_rows_at(rows, i);
		struct store_agent_mail *entry = nb_vec_push(records);

		if (!entry) {
			rc = NB_ERR_NOMEM;
			break;
		}

		memset(entry, 0, sizeof(*entry));
		entry->id = row->id;
		entry->sender = orm_text_copy(row->sender);
		entry->kind = orm_text_copy(row->kind);
		entry->body = orm_text_copy(row->body);
		if (!entry->sender || !entry->kind || !entry->body)
			rc = NB_ERR_NOMEM;
	}

	if (rc != 0)
		agent_store_mail_cleanup(records);

	return rc;
}

int agent_store_mail_ack(struct db *db, int64_t root, const char *recipient, int64_t cursor)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&agent_mail_model, 4), norm_i64(root)),
		norm_eq(norm_col(&agent_mail_model, 5), orm_string(recipient)),
		norm_le(norm_col(&agent_mail_model, 0), norm_i64(cursor)),
	};
	const struct norm_assignment change = {.column = norm_col(&agent_mail_model, 6),
					       .value = norm_i32(1)};
	const struct norm_update_query query = {
		.set = &change,
		.set_count = 1,
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
	};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_update(scope.connection, &agent_mail_model, &query, NULL, NULL,
					   &error),
			       &error);
}

int agent_store_budget_copy(struct db *db, int64_t root, struct store_agent_budget *record)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const struct norm_select_query query = {
		.where = norm_eq(norm_col(&agent_budget_model, 5), norm_i64(root)),
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(db,
				norm_select(scope.connection, &agent_budget_model, &query, NULL,
					    &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	memset(record, 0, sizeof(*record));
	if (norm_rows_count(rows)) {
		const struct agent_budget_row *row = norm_rows_at(rows, 0);

		record->used = row->used;
		record->input_tokens = row->input_tokens;
		record->cached_tokens = row->cached_tokens;
		record->output_tokens = row->output_tokens;
		record->reminder_level = row->reminder_level;
	}

	return 0;
}

int agent_store_budget_put(struct db *db, int64_t root, const struct store_agent_budget *record)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct agent_budget_row row = {
		.root_session_id = root,
		.used = record->used,
		.input_tokens = record->input_tokens,
		.cached_tokens = record->cached_tokens,
		.output_tokens = record->output_tokens,
		.reminder_level = record->reminder_level,
	};
	const size_t key = 5;
	struct norm_term excluded[5] = {0};
	struct norm_assignment changes[5] = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(changes); i++) {
		excluded[i] = norm_term_excluded(norm_col(&agent_budget_model, i));
		changes[i] = (struct norm_assignment){.column = norm_col(&agent_budget_model, i),
						      .expression = &excluded[i]};
	}

	const struct norm_conflict conflict = {
		.action = NORM_CONFLICT_UPDATE,
		.fields = &key,
		.field_count = 1,
		.set = changes,
		.set_count = NB_ARRAY_SIZE(changes),
	};
	const struct norm_insert_options options = {.conflict = &conflict};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_insert(scope.connection, &agent_budget_model, &row, &options,
					   NULL, NULL, &error),
			       &error);
}

int agent_store_history_fork(struct db *db, int64_t source, int64_t destination)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_table_ref history = norm_table(&history_model, NB_STR_LIT("h"));
	const struct norm_table_ref latest = norm_table(&history_model, NB_STR_LIT("latest"));
	const struct norm_expr latest_conditions[] = {
		norm_eq(norm_table_col(&latest, HIST_SESSION_ID), norm_i64(source)),
		norm_eq(norm_table_col(&latest, HIST_ACTIVE), norm_i32(1)),
		norm_eq(norm_table_col(&latest, HIST_KIND), orm_string("assistant_tool_calls")),
	};
	const struct norm_term sequence =
		norm_term_column(norm_table_col(&latest, HIST_SEQUENCE_NO));
	const struct norm_query_field maximum = {norm_term_call(NORM_FUNCTION_MAX, &sequence, 1),
						 0};
	const struct norm_query last_call = {
		.from = &latest,
		.projection = &orm_integer_model,
		.fields = &maximum,
		.field_count = 1,
		.where = norm_and(latest_conditions, NB_ARRAY_SIZE(latest_conditions)),
	};
	const struct norm_term cutoff_arguments[] = {
		norm_term_scalar(&last_call),
		norm_term_value(norm_i64(INT64_MAX)),
	};
	const struct norm_term cutoff = norm_term_call(NORM_FUNCTION_COALESCE, cutoff_arguments,
						       NB_ARRAY_SIZE(cutoff_arguments));
	const struct norm_term current_sequence =
		norm_term_column(norm_table_col(&history, HIST_SEQUENCE_NO));
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&history, HIST_SESSION_ID), norm_i64(source)),
		norm_eq(norm_table_col(&history, HIST_ACTIVE), norm_i32(1)),
		norm_compare_terms(&current_sequence, NORM_COMPARE_LT, &cutoff),
	};
	const size_t fields[] = {
		HIST_SESSION_ID,   HIST_SEQUENCE_NO,  HIST_TURN_ID,
		HIST_KIND,	   HIST_ROLE,	      HIST_CONTENT,
		HIST_PAYLOAD_JSON, HIST_TOOL_CALL_ID, HIST_PROVIDER_CALL_ID,
		HIST_TOOL_NAME,	   HIST_TOKEN_COUNT,  HIST_TRUNCATED,
		HIST_ACTIVE,	   HIST_CREATED_AT,
	};
	struct norm_query_field selected[NB_ARRAY_SIZE(fields)] = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(fields); i++)
		selected[i] = (struct norm_query_field){
			i == 0 ? norm_term_value(norm_i64(destination))
			       : norm_term_column(norm_table_col(&history, fields[i])),
			fields[i]};

	const struct norm_query_order order = {.value = current_sequence};
	const struct norm_query query = {
		.from = &history,
		.projection = &history_model,
		.fields = selected,
		.field_count = NB_ARRAY_SIZE(selected),
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
		.order = &order,
		.order_count = 1,
	};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_insert_select(scope.connection, &history_model, fields,
						  NB_ARRAY_SIZE(fields), &query, NULL, NULL, NULL,
						  &error),
			       &error);
}

int agent_store_event_put(struct db *db, int64_t root, const char *agent_id, const char *event_json)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct agent_event_row row = {
		.root_session_id = root,
		.agent_id = nb_str_from_cstr(agent_id),
		.event_json = nb_str_from_cstr(event_json),
	};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_insert(scope.connection, &agent_event_model, &row, NULL, NULL,
					   NULL, &error),
			       &error);
}

int agent_store_event_latest_copy(struct db *db, int64_t root, const char *agent_id, char **json)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const size_t field = 3;
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&agent_event_model, 1), norm_i64(root)),
		norm_eq(norm_col(&agent_event_model, 2), orm_string(agent_id)),
	};
	const struct norm_order order = {.column = norm_col(&agent_event_model, 0),
					 .direction = NORM_DESC};
	const struct norm_select_query query = {
		.fields = &field,
		.field_count = 1,
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
		.order = &order,
		.order_count = 1,
		.has_limit = true,
		.limit = 1,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(db,
				norm_select(scope.connection, &agent_event_model, &query, NULL,
					    &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return NB_ERR_NOT_FOUND;

	const struct agent_event_row *row = norm_rows_at(rows, 0);

	*json = orm_text_copy(row->event_json);
	return !*json ? NB_ERR_NOMEM : 0;
}

int agent_store_events_copy(struct db *db, int64_t root, const char *agent_id, nb_vec *records)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const size_t field = 3;
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&agent_event_model, 1), norm_i64(root)),
		norm_eq(norm_col(&agent_event_model, 2), orm_string(agent_id)),
	};
	const struct norm_order order = {.column = norm_col(&agent_event_model, 0),
					 .direction = NORM_ASC};
	const struct norm_select_query query = {
		.fields = &field,
		.field_count = 1,
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
		.order = &order,
		.order_count = 1,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};
	int rc = orm_scope_begin(db, &scope);

	nb_vec_init(records, sizeof(char *));
	if (rc == 0)
		rc = orm_status(db,
				norm_select(scope.connection, &agent_event_model, &query, NULL,
					    &rows, &error),
				&error);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct agent_event_row *row = norm_rows_at(rows, i);
		char **entry = nb_vec_push(records);

		if (!entry) {
			rc = NB_ERR_NOMEM;
			break;
		}

		*entry = orm_text_copy(row->event_json);
		if (!*entry)
			rc = NB_ERR_NOMEM;
	}

	if (rc != 0)
		store_text_records_cleanup(records);

	return rc;
}
