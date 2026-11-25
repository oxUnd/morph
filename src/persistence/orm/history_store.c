#include <norm/query.h>
#include "session_models.h"
#include "schema_models.h"
#include "sync_display_models.h"
#include "history_store.h"
#include "history_models.h"
#include "errors.h"
#include <stdio.h>

static const struct norm_field share_fields[] = {
	NORM_FIELD(struct history_row, sequence_no, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct history_row, turn_id, turn_id_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct history_row, kind, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct history_row, content, content_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct history_row, payload_json, payload_json_missing, NORM_TYPE_TEXT,
			    0),
	NORM_NULLABLE_FIELD(struct history_row, tool_name, tool_name_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct history_row, truncated, NORM_TYPE_I32, 0),
	NORM_NULLABLE_FIELD(struct history_row, idempotency_key, idempotency_key_missing,
			    NORM_TYPE_TEXT, 0),
};
static const struct norm_model share_model = NORM_MODEL(struct history_row, "", share_fields);

static const struct norm_field page_fields[] = {
	NORM_FIELD(struct history_row, sequence_no, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct history_row, turn_id, turn_id_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct history_row, kind, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct history_row, role, role_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct history_row, content, content_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct history_row, payload_json, payload_json_missing, NORM_TYPE_TEXT,
			    0),
	NORM_NULLABLE_FIELD(struct history_row, tool_name, tool_name_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct history_row, tool_call_id, tool_call_id_missing, NORM_TYPE_TEXT,
			    0),
	NORM_FIELD(struct history_row, active, NORM_TYPE_I32, 0),
	NORM_FIELD(struct history_row, truncated, NORM_TYPE_I32, 0),
	NORM_FIELD(struct history_row, created_at, NORM_TYPE_I64, 0),
};
static const struct norm_model page_model = NORM_MODEL(struct history_row, "", page_fields);

static const struct norm_field retention_fields[] = {
	NORM_FIELD(struct history_row, id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct history_row, kind, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct history_row, content, content_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct history_row, payload_json, payload_json_missing, NORM_TYPE_TEXT,
			    0),
};
static const struct norm_model retention_model =
	NORM_MODEL(struct history_row, "", retention_fields);

struct context_row {
	int64_t tokens;
	int64_t items;
	int messages;
};
static const struct norm_field context_fields[] = {
	NORM_FIELD(struct context_row, tokens, NORM_TYPE_I64, 0),
	NORM_FIELD(struct context_row, items, NORM_TYPE_I64, 0),
	NORM_FIELD(struct context_row, messages, NORM_TYPE_I32, 0),
};
static const struct norm_model context_model = NORM_MODEL(struct context_row, "", context_fields);

struct snapshot_row {
	nb_str turn_id;
	bool turn_id_missing;
	nb_str provider;
	bool provider_missing;
	nb_str model;
	bool model_missing;
	nb_str input_json;
	bool input_json_missing;
	int64_t created_at;
};
static const struct norm_field snapshot_fields[] = {
	NORM_NULLABLE_FIELD(struct snapshot_row, turn_id, turn_id_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct snapshot_row, provider, provider_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct snapshot_row, model, model_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct snapshot_row, input_json, input_json_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct snapshot_row, created_at, NORM_TYPE_I64, 0),
};
static const struct norm_model snapshot_model =
	NORM_MODEL(struct snapshot_row, "", snapshot_fields);

struct history_message_row {
	int64_t id;
	nb_str role;
	bool role_missing;
	nb_str content;
	bool content_missing;
	nb_str turn_id;
	bool turn_id_missing;
	int64_t created_at;
};
static const struct norm_field message_fields[] = {
	NORM_FIELD(struct history_message_row, id, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct history_message_row, role, role_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct history_message_row, content, content_missing, NORM_TYPE_TEXT,
			    0),
	NORM_NULLABLE_FIELD(struct history_message_row, turn_id, turn_id_missing, NORM_TYPE_TEXT,
			    0),
	NORM_FIELD(struct history_message_row, created_at, NORM_TYPE_I64, 0),
};
static const struct norm_model history_message_projection =
	NORM_MODEL(struct history_message_row, "", message_fields);

struct display_row {
	nb_str type;
	bool type_missing;
	nb_str content;
	bool content_missing;
	nb_str attachments_json;
	bool attachments_json_missing;
	nb_str structured_data;
	bool structured_data_missing;
	nb_str agent_ui_ir;
	bool agent_ui_ir_missing;
	int64_t hitl_verdict;
	bool hitl_verdict_missing;
	int64_t core_message_id;
	bool core_message_id_missing;
	int64_t created_at;
	bool created_at_missing;
	nb_str turn_id;
	bool turn_id_missing;
};
static const struct norm_field display_fields[] = {
	NORM_NULLABLE_FIELD(struct display_row, type, type_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct display_row, content, content_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct display_row, attachments_json, attachments_json_missing,
			    NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct display_row, structured_data, structured_data_missing,
			    NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct display_row, agent_ui_ir, agent_ui_ir_missing, NORM_TYPE_TEXT,
			    0),
	NORM_NULLABLE_FIELD(struct display_row, hitl_verdict, hitl_verdict_missing, NORM_TYPE_I64,
			    0),
	NORM_NULLABLE_FIELD(struct display_row, core_message_id, core_message_id_missing,
			    NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct display_row, created_at, created_at_missing, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct display_row, turn_id, turn_id_missing, NORM_TYPE_TEXT, 0),
};
static const struct norm_model display_model = NORM_MODEL(struct display_row, "", display_fields);

struct trace_row {
	int64_t id;
	nb_str steps_json;
	bool steps_json_missing;
	int64_t created_at;
};
static const struct norm_field trace_fields[] = {
	NORM_FIELD(struct trace_row, id, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct trace_row, steps_json, steps_json_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct trace_row, created_at, NORM_TYPE_I64, 0),
};
static const struct norm_model trace_model = NORM_MODEL(struct trace_row, "", trace_fields);

struct pair_row {
	nb_str answer;
	bool answer_missing;
	nb_str question;
	bool question_missing;
};
static const struct norm_field pair_fields[] = {
	NORM_NULLABLE_FIELD(struct pair_row, answer, answer_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct pair_row, question, question_missing, NORM_TYPE_TEXT, 0),
};
static const struct norm_model pair_model = NORM_MODEL(struct pair_row, "", pair_fields);

static const struct norm_field progress_fields[] = {
	NORM_FIELD(struct history_message_progress, latest_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct history_message_progress, completed_turns, NORM_TYPE_I32, 0),
};
static const struct norm_model progress_model =
	NORM_MODEL(struct history_message_progress, "", progress_fields);

static int history_query(struct db *db, int64_t session_id, const struct norm_query *query,
			 struct norm_rows **rows)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};

	if (session_id <= 0)
		return NB_ERR_INVAL;

	int rc = orm_scope_begin(db, &lease);

	return rc ? rc : orm_read(&lease, query, rows);
}

int history_store_context_tokens(struct db *db, int64_t session_id, int64_t *tokens)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!tokens)
		return NB_ERR_INVAL;

	*tokens = 0;
	const struct norm_table_ref histories = norm_table(&history_model, NB_STR_LIT("h"));
	const struct norm_table_ref messages = norm_table(&message_model, NB_STR_LIT("m"));
	const struct norm_query_field any_fields[] = {
		{.value = norm_term_value(norm_i32(1)), .into = 0}};
	const struct norm_query any = {
		.from = &messages,
		.projection = &orm_integer_model,
		.fields = any_fields,
		.field_count = 1,
		.where = norm_eq(norm_table_col(&messages, MESSAGE_SESSION_ID),
				 norm_i64(session_id))};
	const struct norm_term zero = norm_term_value(norm_i64(0));
	const struct norm_term one = norm_term_value(norm_i32(1));
	const struct norm_term used_tokens =
		norm_term_column(norm_table_col(&histories, HIST_TOKEN_COUNT));
	const struct norm_case active[] = {
		{.when = norm_eq(norm_table_col(&histories, HIST_ACTIVE), norm_i32(1)),
		 .then = &used_tokens}};
	const struct norm_term used = norm_term_case(active, 1, &zero);
	const struct norm_term defaults[] = {norm_term_call(NORM_FUNCTION_SUM, &used, 1), zero};
	const struct norm_case have_messages[] = {{.when = norm_exists(&any), .then = &one}};
	const struct norm_query_field fields[] = {
		{.value = norm_term_call(NORM_FUNCTION_COALESCE, defaults, 2), .into = 0},
		{.value = norm_term_call(NORM_FUNCTION_COUNT, NULL, 0), .into = 1},
		{.value = norm_term_case(have_messages, 1, &zero), .into = 2},
	};
	const struct norm_query query = {
		.from = &histories,
		.projection = &context_model,
		.fields = fields,
		.field_count = 3,
		.where =
			norm_eq(norm_table_col(&histories, HIST_SESSION_ID), norm_i64(session_id))};
	int rc = history_query(db, session_id, &query, &rows);

	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return MORPH_ERR_DB;

	const struct context_row *row = norm_rows_at(rows, 0);

	*tokens = row->tokens;
	return row->items == 0 && row->messages ? NB_ERR_NO_DATA : 0;
}

static int read_text(struct db *db, int64_t session_id, const struct norm_query *query, char **out)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	int rc = history_query(db, session_id, query, &rows);

	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return NB_ERR_NO_DATA;

	const struct orm_text *row = norm_rows_at(rows, 0);
	char *text = orm_text_copy(row->missing ? NB_STR_LIT("") : row->value);

	if (!text)
		return NB_ERR_NOMEM;

	*out = text;
	return 0;
}

int history_store_last_answer_copy(struct db *db, int64_t session_id, char **answer)
{
	int rc;

	if (!answer)
		return NB_ERR_INVAL;

	if (*answer)
		return NB_ERR_EXISTS;

	const struct norm_table_ref history = norm_table(&history_model, NB_STR_LIT("h"));
	const struct norm_term content = norm_term_column(norm_table_col(&history, HIST_CONTENT));
	const struct norm_term length = norm_term_call(NORM_FUNCTION_LENGTH, &content, 1);
	const struct norm_term zero = norm_term_value(norm_i32(0));
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&history, HIST_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_table_col(&history, HIST_KIND), orm_string("assistant_message")),
		norm_compare_terms(&length, NORM_COMPARE_GT, &zero)};
	const struct norm_query_field fields[] = {{.value = content, .into = 0}};
	const struct norm_query_order order = {
		.value = norm_term_column(norm_table_col(&history, HIST_SEQUENCE_NO)),
		.direction = NORM_DESC};
	const struct norm_query query = {.from = &history,
					 .projection = &orm_text_model,
					 .fields = fields,
					 .field_count = 1,
					 .where = norm_and(conditions, 3),
					 .order = &order,
					 .order_count = 1,
					 .has_limit = true,
					 .limit = 1};
	rc = read_text(db, session_id, &query, answer);
	if (rc == NB_ERR_NO_DATA) {
		const struct norm_table_ref messages = norm_table(&message_model, NB_STR_LIT("m"));
		const struct norm_term body =
			norm_term_column(norm_table_col(&messages, MESSAGE_CONTENT));
		const struct norm_term size = norm_term_call(NORM_FUNCTION_LENGTH, &body, 1);
		const struct norm_expr visible[] = {
			norm_eq(norm_table_col(&messages, MESSAGE_SESSION_ID),
				norm_i64(session_id)),
			norm_eq(norm_table_col(&messages, MESSAGE_ROLE), orm_string("assistant")),
			norm_compare_terms(&size, NORM_COMPARE_GT, &zero)};
		const struct norm_query_field selected[] = {{.value = body, .into = 0}};
		const struct norm_query_order recent[] = {
			{.value = norm_term_column(norm_table_col(&messages, MESSAGE_CREATED_AT)),
			 .direction = NORM_DESC},
			{.value = norm_term_column(norm_table_col(&messages, MESSAGE_ID)),
			 .direction = NORM_DESC}};
		const struct norm_query legacy = {.from = &messages,
						  .projection = &orm_text_model,
						  .fields = selected,
						  .field_count = 1,
						  .where = norm_and(visible, 3),
						  .order = recent,
						  .order_count = 2,
						  .has_limit = true,
						  .limit = 1};
		rc = read_text(db, session_id, &legacy, answer);
	}
	return rc;
}

int history_store_previous_question_copy(struct db *db, int64_t session_id, int64_t before_sequence,
					 char **question)
{
	if (!question)
		return NB_ERR_INVAL;

	if (*question)
		return NB_ERR_EXISTS;

	const struct norm_table_ref source = norm_table(&history_model, NB_STR_LIT("h"));
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&source, HIST_SESSION_ID), norm_i64(session_id)),
		norm_lt(norm_table_col(&source, HIST_SEQUENCE_NO), norm_i64(before_sequence)),
		norm_eq(norm_table_col(&source, HIST_KIND), orm_string("user_message"))};
	const struct norm_query_field fields[] = {
		{.value = norm_term_column(norm_table_col(&source, HIST_CONTENT)), .into = 0}};
	const struct norm_query_order order = {
		.value = norm_term_column(norm_table_col(&source, HIST_SEQUENCE_NO)),
		.direction = NORM_DESC};
	const struct norm_query query = {.from = &source,
					 .projection = &orm_text_model,
					 .fields = fields,
					 .field_count = 1,
					 .where = norm_and(conditions, 3),
					 .order = &order,
					 .order_count = 1,
					 .has_limit = true,
					 .limit = 1};

	return read_text(db, session_id, &query, question);
}

int history_store_summary_sources(struct db *db, int64_t session_id, nb_vec *ids)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!ids)
		return NB_ERR_INVAL;

	struct norm_model covered_model = orm_integer_model;
	covered_model.table = NB_STR_LIT("covered");
	const struct norm_model *sources_model = schema_model(SCHEMA_HISTORY_SUMMARY_SOURCES);
	const struct norm_table_ref sources = norm_table(sources_model, NB_STR_LIT("s"));
	const struct norm_table_ref histories = norm_table(&history_model, NB_STR_LIT("m"));
	const struct norm_table_ref covered = norm_table(&covered_model, NB_STR_LIT("c"));
	const struct norm_join base_join = {
		.table = &histories,
		.kind = NORM_JOIN_INNER,
		.on = norm_columns_eq(
			norm_table_col(&histories, HIST_ID),
			norm_table_col(&sources, schema_field(sources_model, "summary_item_id")))};
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&histories, HIST_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_table_col(&histories, HIST_ACTIVE), norm_i32(1)),
		norm_eq(norm_table_col(&histories, HIST_KIND), orm_string("compaction_summary"))};
	const struct norm_query_field fields[] = {
		{.value = norm_term_column(
			 norm_table_col(&sources, schema_field(sources_model, "source_item_id"))),
		 .into = 0}};
	const struct norm_join recursive_join = {
		.table = &covered,
		.kind = NORM_JOIN_INNER,
		.on = norm_columns_eq(
			norm_table_col(&sources, schema_field(sources_model, "summary_item_id")),
			norm_table_col(&covered, 0))};
	const struct norm_query recursive = {.from = &sources,
					     .joins = &recursive_join,
					     .join_count = 1,
					     .projection = &covered_model,
					     .fields = fields,
					     .field_count = 1};
	const struct norm_query_set union_source[] = {
		{.operation = NORM_SET_UNION, .query = &recursive}};
	const struct norm_query base = {.from = &sources,
					.joins = &base_join,
					.join_count = 1,
					.projection = &covered_model,
					.fields = fields,
					.field_count = 1,
					.where = norm_and(conditions, 3),
					.sets = union_source,
					.set_count = 1};
	const struct norm_cte with[] = {
		{.model = &covered_model, .query = &base, .recursive = true}};
	const struct norm_query_field selected[] = {
		{.value = norm_term_column(norm_table_col(&covered, 0)), .into = 0}};
	const struct norm_query query = {.with = with,
					 .with_count = 1,
					 .from = &covered,
					 .projection = &orm_integer_model,
					 .fields = selected,
					 .field_count = 1};
	int rc = history_query(db, session_id, &query, &rows);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct orm_integer *row = norm_rows_at(rows, i);
		int64_t *slot = nb_vec_push(ids);

		if (!slot)
			return NB_ERR_NOMEM;

		*slot = row->value;
	}

	return rc;
}

void history_request_snapshot_cleanup(struct history_request_snapshot *snapshot)
{
	if (!snapshot)
		return;

	free(snapshot->turn_id);
	free(snapshot->provider);
	free(snapshot->model);
	free(snapshot->input_json);
	memset(snapshot, 0, sizeof(*snapshot));
}

static void history_request_snapshot_move(struct history_request_snapshot *destination,
					  struct history_request_snapshot *source)
{
	*destination = *source;
	memset(source, 0, sizeof(*source));
}

int history_store_request_snapshot(struct db *db, int64_t session_id,
				   struct history_request_snapshot *out)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct history_request_snapshot value NB_CLEANUP(history_request_snapshot_cleanup) = {0};

	if (!out)
		return NB_ERR_INVAL;

	if (out->turn_id || out->provider || out->model || out->input_json)
		return NB_ERR_EXISTS;

	const struct norm_model *model = schema_model(SCHEMA_MODEL_REQUEST_SNAPSHOTS);
	const struct norm_table_ref source = norm_table(model, NB_STR_LIT("s"));
	const char *const names[] = {"turn_id", "provider", "model", "input_json", "created_at"};
	struct norm_query_field fields[5] = {0};
	for (size_t i = 0; i < 5; i++)
		fields[i] =
			(struct norm_query_field){.value = norm_term_column(norm_table_col(
							  &source, schema_field(model, names[i]))),
						  .into = i};

	const struct norm_query query = {
		.from = &source,
		.projection = &snapshot_model,
		.fields = fields,
		.field_count = 5,
		.where = norm_eq(norm_table_col(&source, schema_field(model, "session_id")),
				 norm_i64(session_id))};
	int rc = history_query(db, session_id, &query, &rows);

	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return NB_ERR_NO_DATA;

	const struct snapshot_row *row = norm_rows_at(rows, 0);
	value.turn_id = orm_text_copy(row->turn_id_missing ? NB_STR_LIT("") : row->turn_id);
	if (!value.turn_id)
		return NB_ERR_NOMEM;

	value.provider = orm_text_copy(row->provider_missing ? NB_STR_LIT("") : row->provider);
	if (!value.provider)
		return NB_ERR_NOMEM;

	value.model = orm_text_copy(row->model_missing ? NB_STR_LIT("") : row->model);
	if (!value.model)
		return NB_ERR_NOMEM;

	value.input_json =
		orm_text_copy(row->input_json_missing ? NB_STR_LIT("") : row->input_json);
	if (!value.input_json)
		return NB_ERR_NOMEM;

	value.created_at = row->created_at;
	history_request_snapshot_move(out, &value);
	return 0;
}

static int visit_messages(struct db *db, int64_t session_id, int limit,
			  history_message_visitor visitor, void *data)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!visitor)
		return NB_ERR_INVAL;

	const struct norm_table_ref source = norm_table(&message_model, NB_STR_LIT("m"));
	const size_t selected[] = {MESSAGE_ID, MESSAGE_ROLE, MESSAGE_CONTENT, MESSAGE_TURN_ID,
				   MESSAGE_CREATED_AT};
	struct norm_query_field fields[5] = {0};
	for (size_t i = 0; i < 5; i++)
		fields[i] = (struct norm_query_field){
			.value = norm_term_column(norm_table_col(&source, selected[i])), .into = i};

	const struct norm_term body = norm_term_column(norm_table_col(&source, MESSAGE_CONTENT));
	const struct norm_term trimmed = norm_term_named_call(NB_STR_LIT("trim"), &body, 1);
	const struct norm_term length = norm_term_call(NORM_FUNCTION_LENGTH, &trimmed, 1);
	const struct norm_term zero = norm_term_value(norm_i32(0));
	const struct norm_value roles[] = {orm_string("user"), orm_string("assistant")};
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&source, MESSAGE_SESSION_ID), norm_i64(session_id)),
		norm_in(norm_table_col(&source, MESSAGE_ROLE), roles, 2),
		norm_compare_terms(&length, NORM_COMPARE_GT, &zero)};
	const struct norm_query_order chronological[] = {
		{.value = norm_term_column(norm_table_col(&source, MESSAGE_CREATED_AT)),
		 .direction = NORM_ASC},
		{.value = norm_term_column(norm_table_col(&source, MESSAGE_ID)),
		 .direction = NORM_ASC}};
	const struct norm_query_order recent = {
		.value = norm_term_column(norm_table_col(&source, MESSAGE_ID)),
		.direction = NORM_DESC};
	const struct norm_result_options limits = {.max_rows = SIZE_MAX, .max_bytes = SIZE_MAX};
	const struct norm_query query = {.from = &source,
					 .projection = &history_message_projection,
					 .fields = fields,
					 .field_count = 5,
					 .where = norm_and(conditions, limit > 0 ? 3 : 1),
					 .order = limit > 0 ? &recent : chronological,
					 .order_count = limit > 0 ? 1 : 2,
					 .has_limit = limit > 0,
					 .limit = limit > 0 ? (uint64_t)limit : 0,
					 .result_options = &limits};
	int rc = history_query(db, session_id, &query, &rows);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct history_message_row *row = norm_rows_at(rows, i);
		char *content NB_CLEANUP(nb_charp_cleanup) =
			row->content_missing ? NULL : orm_text_copy(row->content);
		char *turn_id NB_CLEANUP(nb_charp_cleanup) =
			row->turn_id_missing ? NULL : orm_text_copy(row->turn_id);
		struct message message = {.id = row->id,
					  .session_id = session_id,
					  .content = content,
					  .turn_id = turn_id,
					  .created_at = row->created_at};

		if ((!row->content_missing && !content) || (!row->turn_id_missing && !turn_id))
			return NB_ERR_NOMEM;

		if (nb_str_copy_to(row->role_missing ? NB_STR_LIT("") : row->role, message.role,
				   sizeof(message.role)) != NB_OK)
			return NB_ERR_RANGE;

		rc = visitor(&message, data);
	}

	return rc < 0 ? rc : 0;
}

int history_store_visit_messages(struct db *db, int64_t session_id, history_message_visitor visitor,
				 void *data)
{
	return visit_messages(db, session_id, 0, visitor, data);
}

int history_store_visit_recent_messages(struct db *db, int64_t session_id, int limit,
					history_message_visitor visitor, void *data)
{
	if (limit <= 0)
		return NB_ERR_INVAL;

	return visit_messages(db, session_id, limit, visitor, data);
}

int history_store_visit_display(struct db *db, int64_t session_id, history_display_visitor visitor,
				void *data)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!visitor)
		return NB_ERR_INVAL;

	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	bool present = false;
	int rc = orm_scope_begin(db, &lease);
	if (rc == 0)
		rc = orm_table_has_column(&lease, "main", "sync_ui_messages", "session_id",
					  &present);

	if (rc != 0 || !present)
		return rc ? rc : NB_ERR_NO_DATA;

	orm_scope_cleanup(&lease);
	const struct norm_table_ref source = norm_table(&sync_display_model, NB_STR_LIT("u"));
	const size_t selected[] = {4, 5, 6, 7, 8, 9, 2, 10, 1};
	struct norm_query_field fields[9] = {0};
	for (size_t i = 0; i < 9; i++)
		fields[i] = (struct norm_query_field){
			.value = norm_term_column(norm_table_col(&source, selected[i])), .into = i};

	const struct norm_query_order order = {
		.value = norm_term_column(norm_table_col(&source, 3)), .direction = NORM_ASC};
	const struct norm_result_options limits = {.max_rows = SIZE_MAX, .max_bytes = SIZE_MAX};
	const struct norm_query query = {
		.from = &source,
		.projection = &display_model,
		.fields = fields,
		.field_count = 9,
		.where = norm_eq(norm_table_col(&source, 0), norm_i64(session_id)),
		.order = &order,
		.order_count = 1,
		.result_options = &limits};
	rc = history_query(db, session_id, &query, &rows);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct display_row *row = norm_rows_at(rows, i);
		char *type NB_CLEANUP(nb_charp_cleanup) =
			row->type_missing ? NULL : orm_text_copy(row->type);

		if (!row->type_missing && !type)
			return NB_ERR_NOMEM;

		char *content NB_CLEANUP(nb_charp_cleanup) =
			row->content_missing ? NULL : orm_text_copy(row->content);

		if (!row->content_missing && !content)
			return NB_ERR_NOMEM;

		char *attachments_json NB_CLEANUP(nb_charp_cleanup) =
			row->attachments_json_missing ? NULL : orm_text_copy(row->attachments_json);

		if (!row->attachments_json_missing && !attachments_json)
			return NB_ERR_NOMEM;

		char *structured_data NB_CLEANUP(nb_charp_cleanup) =
			row->structured_data_missing ? NULL : orm_text_copy(row->structured_data);

		if (!row->structured_data_missing && !structured_data)
			return NB_ERR_NOMEM;

		char *agent_ui_ir NB_CLEANUP(nb_charp_cleanup) =
			row->agent_ui_ir_missing ? NULL : orm_text_copy(row->agent_ui_ir);

		if (!row->agent_ui_ir_missing && !agent_ui_ir)
			return NB_ERR_NOMEM;

		char *turn_id NB_CLEANUP(nb_charp_cleanup) =
			row->turn_id_missing ? NULL : orm_text_copy(row->turn_id);

		if (!row->turn_id_missing && !turn_id)
			return NB_ERR_NOMEM;

		struct history_display_record record = {
			.type = type,
			.content = content,
			.attachments_json = attachments_json,
			.structured_data = structured_data,
			.agent_ui_ir = agent_ui_ir,
			.hitl_verdict = row->hitl_verdict,
			.core_message_id = row->core_message_id,
			.created_at = row->created_at,
			.turn_id = turn_id,
		};

		if (!row->hitl_verdict_missing)
			record.present |= HISTORY_DISPLAY_VERDICT;

		if (!row->core_message_id_missing)
			record.present |= HISTORY_DISPLAY_MESSAGE;

		if (!row->created_at_missing)
			record.present |= HISTORY_DISPLAY_CREATED;

		rc = visitor(&record, data);
	}

	return rc < 0 ? rc : 0;
}

int history_store_visit_traces(struct db *db, int64_t session_id, history_trace_visitor visitor,
			       void *data)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!visitor)
		return NB_ERR_INVAL;

	const struct norm_model *model = schema_model(SCHEMA_REACT_TRACES);
	const struct norm_table_ref source = norm_table(model, NB_STR_LIT("r"));
	const char *const names[] = {"id", "steps_json", "created_at"};
	struct norm_query_field fields[3] = {0};
	for (size_t i = 0; i < 3; i++)
		fields[i] =
			(struct norm_query_field){.value = norm_term_column(norm_table_col(
							  &source, schema_field(model, names[i]))),
						  .into = i};

	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&source, schema_field(model, "session_id")),
			norm_i64(session_id)),
		norm_eq(norm_table_col(&source, schema_field(model, "aborted")), norm_i64(0))};
	const struct norm_query_order order[] = {
		{.value = norm_term_column(
			 norm_table_col(&source, schema_field(model, "created_at"))),
		 .direction = NORM_ASC},
		{.value = norm_term_column(norm_table_col(&source, schema_field(model, "id"))),
		 .direction = NORM_ASC}};
	const struct norm_result_options limits = {.max_rows = SIZE_MAX, .max_bytes = SIZE_MAX};
	const struct norm_query query = {.from = &source,
					 .projection = &trace_model,
					 .fields = fields,
					 .field_count = 3,
					 .where = norm_and(conditions, 2),
					 .order = order,
					 .order_count = 2,
					 .result_options = &limits};
	int rc = history_query(db, session_id, &query, &rows);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct trace_row *row = norm_rows_at(rows, i);
		char *steps NB_CLEANUP(nb_charp_cleanup) =
			row->steps_json_missing ? NULL : orm_text_copy(row->steps_json);

		if (!row->steps_json_missing && !steps)
			return NB_ERR_NOMEM;

		struct history_trace_record record = {
			.id = row->id, .steps_json = steps, .created_at = row->created_at};

		rc = visitor(&record, data);
	}

	return rc < 0 ? rc : 0;
}

static int visit_share_items(struct db *db, int64_t session_id, const char *turn_id,
			     int64_t sequence, history_item_visitor visitor, void *data)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!visitor)
		return NB_ERR_INVAL;

	const struct norm_table_ref source = norm_table(&history_model, NB_STR_LIT("h"));
	const size_t selected[] = {HIST_SEQUENCE_NO, HIST_TURN_ID,	  HIST_KIND,
				   HIST_CONTENT,     HIST_PAYLOAD_JSON,	  HIST_TOOL_NAME,
				   HIST_TRUNCATED,   HIST_IDEMPOTENCY_KEY};
	struct norm_query_field fields[8] = {0};
	for (size_t i = 0; i < 8; i++)
		fields[i] = (struct norm_query_field){
			.value = norm_term_column(norm_table_col(&source, selected[i])), .into = i};

	const struct norm_term content = norm_term_column(norm_table_col(&source, HIST_CONTENT));
	const struct norm_term length = norm_term_call(NORM_FUNCTION_LENGTH, &content, 1);
	const struct norm_term zero = norm_term_value(norm_i32(0));
	const struct norm_value kinds[] = {orm_string("user_message"), orm_string("tool_result")};
	const struct norm_expr answers[] = {
		norm_eq(norm_table_col(&source, HIST_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_table_col(&source, HIST_KIND), orm_string("assistant_message")),
		norm_compare_terms(&length, NORM_COMPARE_GT, &zero)};
	const struct norm_expr turn[] = {
		norm_eq(norm_table_col(&source, HIST_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_table_col(&source, HIST_TURN_ID), orm_string(turn_id)),
		norm_lt(norm_table_col(&source, HIST_SEQUENCE_NO), norm_i64(sequence)),
		norm_in(norm_table_col(&source, HIST_KIND), kinds, 2)};
	const struct norm_query_order order = {
		.value = norm_term_column(norm_table_col(&source, HIST_SEQUENCE_NO)),
		.direction = turn_id ? NORM_ASC : NORM_DESC};
	const struct norm_query query = {.from = &source,
					 .projection = &share_model,
					 .fields = fields,
					 .field_count = 8,
					 .where =
						 turn_id ? norm_and(turn, 4) : norm_and(answers, 3),
					 .order = &order,
					 .order_count = 1};
	int rc = history_query(db, session_id, &query, &rows);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct history_row *row = norm_rows_at(rows, i);
		struct model_history_item item NB_CLEANUP(orm_history_item_cleanup) = {0};

		rc = orm_history_item_copy(row, &item);
		if (rc == 0)
			rc = visitor(&item, data);
	}

	return rc < 0 ? rc : 0;
}

int history_store_visit_answers(struct db *db, int64_t session_id, history_item_visitor visitor,
				void *data)
{
	return visit_share_items(db, session_id, NULL, 0, visitor, data);
}

int history_store_visit_page(struct db *db, int64_t session_id, int64_t before_sequence, int limit,
			     history_item_visitor visitor, void *data)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	if (!visitor || before_sequence < 0 || limit < 1 || limit > 1001)
		return NB_ERR_INVAL;

	struct norm_model page_source_model = page_model;
	page_source_model.table = NB_STR_LIT("page_rows");
	const struct norm_table_ref histories = norm_table(&history_model, NB_STR_LIT("h"));
	const struct norm_table_ref messages = norm_table(&message_model, NB_STR_LIT("m"));
	const struct norm_table_ref page = norm_table(&page_source_model, NB_STR_LIT("p"));
	const size_t indices[] = {HIST_SEQUENCE_NO, HIST_TURN_ID,      HIST_KIND,
				  HIST_ROLE,	    HIST_CONTENT,      HIST_PAYLOAD_JSON,
				  HIST_TOOL_NAME,   HIST_TOOL_CALL_ID, HIST_ACTIVE,
				  HIST_TRUNCATED,   HIST_CREATED_AT};
	struct norm_query_field history_fields[11] = {0};
	struct norm_query_field selected[11] = {0};
	for (size_t i = 0; i < 11; i++) {
		history_fields[i] = (struct norm_query_field){
			.value = norm_term_column(norm_table_col(&histories, indices[i])),
			.into = i};
		selected[i] = (struct norm_query_field){
			.value = norm_term_column(norm_table_col(&page, i)), .into = i};
	}

	const struct norm_query_field any_fields[] = {
		{.value = norm_term_value(norm_i32(1)), .into = 0}};
	const struct norm_query any = {.from = &histories,
				       .projection = &orm_integer_model,
				       .fields = any_fields,
				       .field_count = 1,
				       .where = norm_eq(norm_table_col(&histories, HIST_SESSION_ID),
							norm_i64(session_id))};
	const struct norm_expr exists = norm_exists(&any);
	const struct norm_expr legacy_conditions[] = {
		norm_eq(norm_table_col(&messages, MESSAGE_SESSION_ID), norm_i64(session_id)),
		norm_not(&exists),
		before_sequence
			? norm_lt(norm_table_col(&messages, MESSAGE_ID), norm_i64(before_sequence))
			: (struct norm_expr){0}};
	const struct norm_query_field legacy_fields[] = {
		{.value = norm_term_column(norm_table_col(&messages, MESSAGE_ID)), .into = 0},
		{.value = norm_term_column(norm_table_col(&messages, MESSAGE_TURN_ID)), .into = 1},
		{.value = norm_term_value(orm_string("legacy_message")), .into = 2},
		{.value = norm_term_column(norm_table_col(&messages, MESSAGE_ROLE)), .into = 3},
		{.value = norm_term_column(norm_table_col(&messages, MESSAGE_CONTENT)), .into = 4},
		{.value = norm_term_value(norm_null()), .into = 5},
		{.value = norm_term_value(orm_string("")), .into = 6},
		{.value = norm_term_value(norm_null()), .into = 7},
		{.value = norm_term_value(norm_i32(1)), .into = 8},
		{.value = norm_term_value(norm_i32(0)), .into = 9},
		{.value = norm_term_column(norm_table_col(&messages, MESSAGE_CREATED_AT)),
		 .into = 10},
	};
	const struct norm_query legacy = {
		.from = &messages,
		.projection = &page_source_model,
		.fields = legacy_fields,
		.field_count = 11,
		.where = norm_and(legacy_conditions, before_sequence ? 3 : 2)};
	const struct norm_query_set sets[] = {{.operation = NORM_SET_UNION_ALL, .query = &legacy}};
	const struct norm_expr history_conditions[] = {
		norm_eq(norm_table_col(&histories, HIST_SESSION_ID), norm_i64(session_id)),
		before_sequence ? norm_lt(norm_table_col(&histories, HIST_SEQUENCE_NO),
					  norm_i64(before_sequence))
				: (struct norm_expr){0}};
	const struct norm_query union_rows = {
		.from = &histories,
		.projection = &page_source_model,
		.fields = history_fields,
		.field_count = 11,
		.where = norm_and(history_conditions, before_sequence ? 2 : 1),
		.sets = sets,
		.set_count = 1};
	const struct norm_cte with[] = {{.model = &page_source_model, .query = &union_rows}};
	const struct norm_query_order order = {.value = norm_term_column(norm_table_col(&page, 0)),
					       .direction = NORM_DESC};
	const struct norm_query query = {.with = with,
					 .with_count = 1,
					 .from = &page,
					 .projection = &page_model,
					 .fields = selected,
					 .field_count = 11,
					 .order = &order,
					 .order_count = 1,
					 .has_limit = true,
					 .limit = (uint64_t)limit};
	int rc = history_query(db, session_id, &query, &rows);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct history_row *row = norm_rows_at(rows, i);
		struct model_history_item item NB_CLEANUP(orm_history_item_cleanup) = {0};

		rc = orm_history_item_copy(row, &item);
		if (rc == 0) {
			item.session_id = session_id;
			rc = visitor(&item, data);
		}
	}

	return rc < 0 ? rc : 0;
}

int history_store_visit_turn(struct db *db, int64_t session_id, const char *turn_id,
			     int64_t before_sequence, history_item_visitor visitor, void *data)
{
	if (!turn_id)
		return NB_ERR_INVAL;

	return visit_share_items(db, session_id, turn_id, before_sequence, visitor, data);
}

int history_store_legacy_answer_copy(struct db *db, int64_t session_id, char **answer,
				     char **question)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	char *answer_value NB_CLEANUP(nb_charp_cleanup) = NULL;
	char *question_value NB_CLEANUP(nb_charp_cleanup) = NULL;

	if (!answer || !question || answer == question)
		return NB_ERR_INVAL;

	if (*answer || *question)
		return NB_ERR_EXISTS;

	const struct norm_table_ref answers = norm_table(&message_model, NB_STR_LIT("a"));
	const struct norm_table_ref users = norm_table(&message_model, NB_STR_LIT("u"));
	const struct norm_table_ref candidates = norm_table(&message_model, NB_STR_LIT("c"));
	const struct norm_table_ref histories = norm_table(&history_model, NB_STR_LIT("h"));
	const struct norm_term candidate_time =
		norm_term_column(norm_table_col(&candidates, MESSAGE_CREATED_AT));
	const struct norm_term answer_time =
		norm_term_column(norm_table_col(&answers, MESSAGE_CREATED_AT));
	const struct norm_term candidate_id =
		norm_term_column(norm_table_col(&candidates, MESSAGE_ID));
	const struct norm_term answer_id = norm_term_column(norm_table_col(&answers, MESSAGE_ID));
	const struct norm_expr same_time[] = {
		norm_compare_terms(&candidate_time, NORM_COMPARE_EQ, &answer_time),
		norm_compare_terms(&candidate_id, NORM_COMPARE_LT, &answer_id)};
	const struct norm_expr earlier[] = {
		norm_compare_terms(&candidate_time, NORM_COMPARE_LT, &answer_time),
		norm_and(same_time, 2)};
	const struct norm_expr eligible[] = {
		norm_columns_eq(norm_table_col(&candidates, MESSAGE_SESSION_ID),
				norm_table_col(&answers, MESSAGE_SESSION_ID)),
		norm_eq(norm_table_col(&candidates, MESSAGE_ROLE), orm_string("user")),
		norm_or(earlier, 2)};
	const struct norm_query_field candidate_fields[] = {{.value = candidate_id, .into = 0}};
	const struct norm_query_order candidate_order[] = {
		{.value = candidate_time, .direction = NORM_DESC},
		{.value = candidate_id, .direction = NORM_DESC}};
	const struct norm_query candidate = {.from = &candidates,
					     .projection = &orm_integer_model,
					     .fields = candidate_fields,
					     .field_count = 1,
					     .where = norm_and(eligible, 3),
					     .order = candidate_order,
					     .order_count = 2,
					     .has_limit = true,
					     .limit = 1};
	const struct norm_term selected = norm_term_scalar(&candidate);
	const struct norm_term user_id = norm_term_column(norm_table_col(&users, MESSAGE_ID));
	const struct norm_join join = {
		.table = &users,
		.kind = NORM_JOIN_INNER,
		.on = norm_compare_terms(&user_id, NORM_COMPARE_EQ, &selected)};
	const struct norm_query_field any_fields[] = {
		{.value = norm_term_value(norm_i32(1)), .into = 0}};
	const struct norm_query any = {
		.from = &histories,
		.projection = &orm_integer_model,
		.fields = any_fields,
		.field_count = 1,
		.where = norm_columns_eq(norm_table_col(&histories, HIST_SESSION_ID),
					 norm_table_col(&answers, MESSAGE_SESSION_ID))};
	const struct norm_expr existing = norm_exists(&any);
	const struct norm_term content =
		norm_term_column(norm_table_col(&answers, MESSAGE_CONTENT));
	const struct norm_term length = norm_term_call(NORM_FUNCTION_LENGTH, &content, 1);
	const struct norm_term zero = norm_term_value(norm_i32(0));
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&answers, MESSAGE_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_table_col(&answers, MESSAGE_ROLE), orm_string("assistant")),
		norm_compare_terms(&length, NORM_COMPARE_GT, &zero), norm_not(&existing)};
	const struct norm_query_field fields[] = {
		{.value = content, .into = 0},
		{.value = norm_term_column(norm_table_col(&users, MESSAGE_CONTENT)), .into = 1}};
	const struct norm_query_order order[] = {{.value = answer_time, .direction = NORM_DESC},
						 {.value = answer_id, .direction = NORM_DESC}};
	const struct norm_query query = {.from = &answers,
					 .joins = &join,
					 .join_count = 1,
					 .projection = &pair_model,
					 .fields = fields,
					 .field_count = 2,
					 .where = norm_and(conditions, 4),
					 .order = order,
					 .order_count = 2,
					 .has_limit = true,
					 .limit = 1};
	int rc = history_query(db, session_id, &query, &rows);

	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return NB_ERR_NO_DATA;

	const struct pair_row *row = norm_rows_at(rows, 0);

	answer_value = orm_text_copy(row->answer_missing ? NB_STR_LIT("") : row->answer);
	question_value = orm_text_copy(row->question_missing ? NB_STR_LIT("") : row->question);
	if (!answer_value || !question_value)
		return NB_ERR_NOMEM;

	*answer = nb_charp_steal(&answer_value);
	*question = nb_charp_steal(&question_value);
	return 0;
}

int history_store_visible_progress(struct db *db, int64_t session_id,
				   struct history_message_progress *out)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!out)
		return NB_ERR_INVAL;

	memset(out, 0, sizeof(*out));
	const struct norm_table_ref source = norm_table(&message_model, NB_STR_LIT("m"));
	const struct norm_term id = norm_term_column(norm_table_col(&source, MESSAGE_ID));
	const struct norm_term zero = norm_term_value(norm_i32(0));
	const struct norm_term one = norm_term_value(norm_i32(1));
	const struct norm_case completed[] = {
		{.when = norm_eq(norm_table_col(&source, MESSAGE_ROLE), orm_string("assistant")),
		 .then = &one}};
	const struct norm_term assistant = norm_term_case(completed, 1, &zero);
	const struct norm_term id_default[] = {norm_term_call(NORM_FUNCTION_MAX, &id, 1), zero};
	const struct norm_term count_default[] = {norm_term_call(NORM_FUNCTION_SUM, &assistant, 1),
						  zero};
	const struct norm_query_field fields[] = {
		{.value = norm_term_call(NORM_FUNCTION_COALESCE, id_default, 2), .into = 0},
		{.value = norm_term_call(NORM_FUNCTION_COALESCE, count_default, 2), .into = 1}};
	const struct norm_term content = norm_term_column(norm_table_col(&source, MESSAGE_CONTENT));
	const struct norm_term trimmed = norm_term_named_call(NB_STR_LIT("trim"), &content, 1);
	const struct norm_term length = norm_term_call(NORM_FUNCTION_LENGTH, &trimmed, 1);
	const struct norm_value roles[] = {orm_string("user"), orm_string("assistant")};
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&source, MESSAGE_SESSION_ID), norm_i64(session_id)),
		norm_in(norm_table_col(&source, MESSAGE_ROLE), roles, 2),
		norm_compare_terms(&length, NORM_COMPARE_GT, &zero)};
	const struct norm_query query = {.from = &source,
					 .projection = &progress_model,
					 .fields = fields,
					 .field_count = 2,
					 .where = norm_and(conditions, 3)};
	int rc = history_query(db, session_id, &query, &rows);

	if (rc == 0 && norm_rows_count(rows))
		*out = *(const struct history_message_progress *)norm_rows_at(rows, 0);

	return rc;
}

int history_store_update_item(struct db *db, int64_t id, int active, int token_count)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};

	if (id <= 0 || token_count < 0)
		return NB_ERR_INVAL;

	const struct norm_assignment set[] = {
		{.column = norm_col(&history_model, HIST_ACTIVE), .value = norm_i32(active)},
		{.column = norm_col(&history_model, HIST_TOKEN_COUNT),
		 .value = norm_i32(token_count)}};
	const struct norm_update_query query = {
		.set = set,
		.set_count = 2,
		.where = norm_eq(norm_col(&history_model, HIST_ID), norm_i64(id))};
	int rc = orm_scope_begin(db, &lease);

	return rc ? rc : orm_update_rows(&lease, &history_model, set, 2, query.where, NULL);
}

int history_store_refresh_retention_tokens(struct db *db, int64_t session_id,
					   history_token_counter count, void *data)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!count || session_id <= 0)
		return NB_ERR_INVAL;

	int rc = orm_scope_begin(db, &lease);
	const struct norm_table_ref answers = norm_table(&history_model, NB_STR_LIT("a"));
	const struct norm_table_ref users = norm_table(&history_model, NB_STR_LIT("u"));
	const struct norm_expr user_conditions[] = {
		norm_columns_eq(norm_table_col(&users, HIST_SESSION_ID),
				norm_table_col(&answers, HIST_SESSION_ID)),
		norm_columns_eq(norm_table_col(&users, HIST_TURN_ID),
				norm_table_col(&answers, HIST_TURN_ID)),
		norm_eq(norm_table_col(&users, HIST_ACTIVE), norm_i32(1)),
		norm_eq(norm_table_col(&users, HIST_KIND), orm_string("user_message"))};
	const struct norm_query_field any_fields[] = {
		{.value = norm_term_value(norm_i32(1)), .into = 0}};
	const struct norm_query any = {.from = &users,
				       .projection = &orm_integer_model,
				       .fields = any_fields,
				       .field_count = 1,
				       .where = norm_and(user_conditions, 4)};
	const struct norm_expr answer_conditions[] = {
		norm_eq(norm_table_col(&answers, HIST_KIND), orm_string("assistant_message")),
		norm_exists(&any)};
	const struct norm_expr selected[] = {
		norm_eq(norm_table_col(&answers, HIST_ACTIVE), norm_i32(1)),
		norm_and(answer_conditions, 2)};
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&answers, HIST_SESSION_ID), norm_i64(session_id)),
		norm_or(selected, 2)};
	const size_t indices[] = {HIST_ID, HIST_KIND, HIST_CONTENT, HIST_PAYLOAD_JSON};
	struct norm_query_field fields[4] = {0};
	for (size_t i = 0; i < 4; i++)
		fields[i] = (struct norm_query_field){
			.value = norm_term_column(norm_table_col(&answers, indices[i])), .into = i};

	const struct norm_query query = {.from = &answers,
					 .projection = &retention_model,
					 .fields = fields,
					 .field_count = 4,
					 .where = norm_and(conditions, 2)};
	if (rc == 0)
		rc = orm_read(&lease, &query, &rows);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct history_row *row = norm_rows_at(rows, i);
		struct model_history_item item NB_CLEANUP(orm_history_item_cleanup) = {0};

		rc = orm_history_item_copy(row, &item);
		if (rc != 0)
			break;

		item.session_id = session_id;
		int tokens = count(&item, data);

		if (tokens < 0)
			return tokens;

		const struct norm_assignment set[] = {
			{.column = norm_col(&history_model, HIST_TOKEN_COUNT),
			 .value = norm_i32(tokens)}};
		const struct norm_update_query update = {
			.set = set,
			.set_count = 1,
			.where = norm_eq(norm_col(&history_model, HIST_ID), norm_i64(item.id))};

		rc = orm_update_rows(&lease, &history_model, set, 1, update.where, NULL);
	}

	return rc;
}

struct history_source_row {
	int64_t summary_item_id;
	int64_t source_item_id;
};
static const struct norm_field history_source_fields[] = {
	NORM_FIELD(struct history_source_row, summary_item_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct history_source_row, source_item_id, NORM_TYPE_I64, 0),
};
static const struct norm_model history_source_model =
	NORM_MODEL(struct history_source_row, "history_summary_sources", history_source_fields);

int history_store_add_summary_sources(struct db *db, int64_t summary_id, const int64_t *ids,
				      size_t count)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};

	if (summary_id <= 0 || (count && !ids))
		return NB_ERR_INVAL;

	int rc = orm_scope_begin(db, &lease);

	for (size_t i = 0; rc == 0 && i < count; i++) {
		const struct history_source_row row = {.summary_item_id = summary_id,
						       .source_item_id = ids[i]};

		rc = orm_insert_row(&lease, &history_source_model, &row, NULL, NULL);
	}

	return rc;
}

int history_store_last_sequence(struct db *db, int64_t session_id, int64_t *sequence)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!sequence)
		return NB_ERR_INVAL;

	*sequence = 0;
	const struct norm_table_ref source = norm_table(&history_model, NB_STR_LIT("h"));
	const struct norm_term sequence_value =
		norm_term_column(norm_table_col(&source, HIST_SEQUENCE_NO));
	const struct norm_term defaults[] = {norm_term_call(NORM_FUNCTION_MAX, &sequence_value, 1),
					     norm_term_value(norm_i64(0))};
	const struct norm_query_field fields[] = {
		{.value = norm_term_call(NORM_FUNCTION_COALESCE, defaults, 2), .into = 0}};
	const struct norm_query query = {
		.from = &source,
		.projection = &orm_integer_model,
		.fields = fields,
		.field_count = 1,
		.where = norm_eq(norm_table_col(&source, HIST_SESSION_ID), norm_i64(session_id))};
	int rc = history_query(db, session_id, &query, &rows);

	if (rc == 0 && norm_rows_count(rows)) {
		const struct orm_integer *row = norm_rows_at(rows, 0);

		*sequence = row->value;
	}

	return rc;
}
