#include <norm/query.h>
#include "session_models.h"
#include "schema_models.h"
#include "session_history.h"
#include "history_models.h"
#include "errors.h"
#include <limits.h>
#include <stdio.h>
#include <time.h>

static int history_append(struct orm_scope *scope, const struct model_history_insert *item,
			  int64_t *out_id)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_write_result result = {0};
	const struct norm_table_ref source = norm_table(&history_model, NB_STR_LIT("h"));
	const struct norm_term sequence =
		norm_term_column(norm_table_col(&source, HIST_SEQUENCE_NO));
	const struct norm_term defaults[] = {norm_term_call(NORM_FUNCTION_MAX, &sequence, 1),
					     norm_term_value(norm_i64(0))};
	const struct norm_query_field selected = {
		.value = norm_term_call(NORM_FUNCTION_COALESCE, defaults, 2), .into = 0};
	const struct norm_query query = {
		.from = &source, .projection = &orm_integer_model,
		.fields = &selected, .field_count = 1,
		.where = norm_eq(norm_table_col(&source, HIST_SESSION_ID),
				 norm_i64(item->session_id))};
	int rc = orm_read(scope, &query, &rows);

	if (rc != NB_OK)
		return rc;

	const struct orm_integer *last = norm_rows_at(rows, 0);

	if (!last || last->value == INT64_MAX)
		return NB_ERR_RANGE;

	const struct history_row row = {.session_id = item->session_id,
					.sequence_no = last->value + 1,
					.turn_id = nb_str_from_cstr(item->turn_id),
					.turn_id_missing = !item->turn_id,
					.kind = nb_str_from_cstr(item->kind),
					.role = nb_str_from_cstr(item->role),
					.role_missing = !item->role,
					.content = nb_str_from_cstr(item->content),
					.content_missing = !item->content,
					.payload_json = nb_str_from_cstr(item->payload_json),
					.payload_json_missing = !item->payload_json,
					.tool_call_id = nb_str_from_cstr(item->tool_call_id),
					.tool_call_id_missing = !item->tool_call_id,
					.provider_call_id =
						nb_str_from_cstr(item->provider_call_id),
					.provider_call_id_missing = !item->provider_call_id,
					.tool_name = nb_str_from_cstr(item->tool_name),
					.tool_name_missing = !item->tool_name,
					.idempotency_key = nb_str_from_cstr(item->idempotency_key),
					.idempotency_key_missing = !item->idempotency_key,
					.token_count = item->token_count,
					.truncated = !!item->truncated,
					.active = !!item->active,
					.created_at = (int64_t)time(NULL)};
	const struct norm_conflict ignore = {.action = NORM_CONFLICT_NOTHING};
	const struct norm_insert_options options = {.conflict = &ignore};

	rc = orm_insert_row(scope, &history_model, &row, &options, &result);
	if (rc != NB_OK)
		return rc;

	if (result.affected_rows) {
		if (!result.has_insert_id)
			return MORPH_ERR_DB;

		if (out_id)
			*out_id = result.insert_id;

		return NB_OK;
	}

	if (!item->idempotency_key)
		return MORPH_ERR_DB;

	orm_rows_cleanup(&rows);
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&history_model, HIST_SESSION_ID), norm_i64(item->session_id)),
		norm_eq(norm_col(&history_model, HIST_IDEMPOTENCY_KEY),
			orm_string(item->idempotency_key))};
	const size_t field = HIST_ID;
	const struct norm_select_query lookup = {
		.where = norm_and(conditions, 2), .fields = &field, .field_count = 1};

	rc = orm_select_rows(scope, &history_model, &lookup, &rows);
	if (rc != NB_OK)
		return rc;

	const struct history_row *existing = norm_rows_at(rows, 0);

	if (!existing)
		return MORPH_ERR_DB;

	if (out_id)
		*out_id = existing->id;

	return NB_OK;
}

int model_history_add(struct db *db, const struct model_history_insert *item, int64_t *out_id)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};

	if (!item || item->session_id <= 0 || !item->kind || !item->kind[0])
		return NB_ERR_INVAL;

	int rc = orm_scope_begin(db, &scope);

	if (rc != NB_OK)
		return rc;

	/* History includes cancellation observations and must survive the turn's
	 * cancellation. Keep its deadline while committing this durable record. */
	scope.call.cancel = NULL;
	rc = orm_savepoint_begin(&scope, "history_append");
	if (rc != NB_OK)
		return rc;

	int64_t inserted = 0;

	rc = history_append(&scope, item, &inserted);
	rc = orm_savepoint_finish(&scope, "history_append", rc);
	if (rc == NB_OK && out_id)
		*out_id = inserted;

	return rc;
}

static void history_list_cleanup(struct model_history_item **owner)
{
	model_history_free_list(*owner);
	*owner = NULL;
}

static struct model_history_item *history_list_steal(struct model_history_item **owner)
{
	struct model_history_item *result = *owner;

	*owner = NULL;
	return result;
}

struct model_history_item *model_history_list(struct db *db, int64_t session_id, int active_only,
					      int *count)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct model_history_item *head NB_CLEANUP(history_list_cleanup) = NULL;
	struct model_history_item *tail = NULL;

	if (count)
		*count = -1;

	if (session_id <= 0 || !count)
		return NULL;

	struct norm_error error = {0};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&history_model, HIST_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_col(&history_model, HIST_ACTIVE), norm_i32(1))};
	const struct norm_order order[] = {
		{.column = norm_col(&history_model, HIST_SEQUENCE_NO), .direction = NORM_ASC},
		{.column = norm_col(&history_model, HIST_ID), .direction = NORM_ASC}};
	const struct norm_select_query query = {.where = norm_and(conditions, active_only ? 2 : 1),
						.order = order,
						.order_count = 2,
						.max_rows = SIZE_MAX,
						.max_bytes = SIZE_MAX};
	int rc = orm_scope_begin(db, &lease);

	if (rc == 0)
		rc = orm_status(db,
				norm_select(lease.connection, &history_model, &query,
					    orm_scope_call(&lease), &rows, &error),
				&error);

	if (rc != 0 || norm_rows_count(rows) > INT_MAX)
		return NULL;

	for (size_t i = 0; i < norm_rows_count(rows); i++) {
		struct model_history_item *item NB_CLEANUP(history_list_cleanup) =
			calloc(1, sizeof(*item));

		if (!item)
			return NULL;

		rc = orm_history_item_copy(norm_rows_at(rows, i), item);
		if (rc != 0)
			return NULL;

		struct model_history_item *node = history_list_steal(&item);

		if (tail)
			tail->next = node;
		else
			head = node;

		tail = node;
	}

	*count = (int)norm_rows_count(rows);
	return history_list_steal(&head);
}

void model_history_free_list(struct model_history_item *head)
{
	while (head) {
		struct model_history_item *next = head->next;

		orm_history_item_cleanup(head);
		free(head);
		head = next;
	}
}

static int history_count(struct db *db, int64_t session_id, const struct norm_model *model,
			 bool active)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};

	if (session_id <= 0)
		return NB_ERR_INVAL;

	const struct norm_table_ref source = norm_table(model, NB_STR_LIT("h"));
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&source, schema_field(model, "session_id")),
			norm_i64(session_id)),
		active ? norm_eq(norm_table_col(&source, HIST_ACTIVE), norm_i32(1))
		       : (struct norm_expr){0}};
	const struct norm_query_field fields[] = {
		{.value = norm_term_call(NORM_FUNCTION_COUNT, NULL, 0), .into = 0}};
	const struct norm_query query = {.from = &source,
					 .projection = &orm_integer_model,
					 .fields = fields,
					 .field_count = 1,
					 .where = norm_and(conditions, active ? 2 : 1)};
	int rc = orm_scope_begin(db, &lease);

	if (rc == 0)
		rc = orm_status(db,
				norm_query_select(lease.connection, &query, orm_scope_call(&lease),
						  &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	const struct orm_integer *row = norm_rows_at(rows, 0);

	return !row ? 0 : row->value > INT_MAX ? NB_ERR_RANGE : (int)row->value;
}

int model_history_count(struct db *db, int64_t session_id, int active_only)
{
	return history_count(db, session_id, &history_model, active_only != 0);
}

static int history_deactivate(struct db *db, int64_t session_id, const char *turn_id, int keep_user)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};

	if (session_id <= 0 || !turn_id || !turn_id[0])
		return NB_ERR_INVAL;

	struct norm_expr conditions[] = {
		norm_eq(norm_col(&history_model, 1), norm_i64(session_id)),
		norm_eq(norm_col(&history_model, 3), orm_string(turn_id)),
		norm_eq(norm_col(&history_model, 14), norm_i32(1)),
		norm_ne(norm_col(&history_model, 4), orm_string("user_message")),
	};
	struct norm_assignment assignment = {.column = norm_col(&history_model, 14),
					     .value = norm_i32(0)};
	struct norm_update_query query = {
		.set = &assignment,
		.set_count = 1,
		.where = norm_and(conditions, keep_user ? 4 : 3),
	};
	struct norm_error error = {0};
	int rc = orm_scope_begin(db, &lease);

	return rc ? rc
		  : orm_status(db,
			       norm_update(lease.connection, &history_model, &query,
					   orm_scope_call(&lease), NULL, &error),
			       &error);
}

int model_history_deactivate_turn(struct db *db, int64_t session_id, const char *turn_id)
{
	return history_deactivate(db, session_id, turn_id, 0);
}

int model_history_deactivate_turn_except_user(struct db *db, int64_t session_id,
					      const char *turn_id)
{
	return history_deactivate(db, session_id, turn_id, 1);
}

int model_history_compaction_count(struct db *db, int64_t session_id)
{
	return history_count(db, session_id, schema_model(SCHEMA_HISTORY_COMPACTIONS), false);
}

int model_history_compaction_attempt_add(struct db *db, int64_t session_id, const char *turn_id,
					 const char *trigger_kind, const char *status,
					 int input_tokens, int output_tokens, int error_code,
					 const char *error_text)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};

	if (session_id <= 0 || !trigger_kind || !status)
		return NB_ERR_INVAL;

	const struct norm_model *target = schema_model(SCHEMA_HISTORY_COMPACTION_ATTEMPTS);
	const char *const names[] = {"session_id", "turn_id",	   "trigger_kind",
				     "status",	   "input_tokens", "output_tokens",
				     "error_code", "error_text",   "created_at"};
	const struct norm_value values[] = {
		norm_i64(session_id), orm_string(turn_id),    orm_string(trigger_kind),
		orm_string(status),   norm_i32(input_tokens), norm_i32(output_tokens),
		norm_i32(error_code), orm_string(error_text), norm_i64((int64_t)time(NULL))};
	struct norm_field fields[9] = {0};
	struct norm_query_field columns[9] = {0};
	size_t targets[9] = {0};
	for (size_t i = 0; i < 9; i++) {
		targets[i] = schema_field(target, names[i]);
		fields[i] = target->fields[targets[i]];
		columns[i] =
			(struct norm_query_field){.value = norm_term_value(values[i]), .into = i};
	}

	const struct norm_model projection = {.size = target->size,
					      .alignment = target->alignment,
					      .fields = fields,
					      .field_count = 9};
	const struct norm_query query = {
		.projection = &projection, .fields = columns, .field_count = 9};
	struct norm_error error = {0};
	int rc = orm_scope_begin(db, &lease);

	return rc ? rc
		  : orm_status(db,
			       norm_insert_select(lease.connection, target, targets, 9, &query,
						  NULL, orm_scope_call(&lease), NULL, &error),
			       &error);
}

int model_history_migrate_messages(struct db *db, int64_t session_id)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};

	if (session_id <= 0)
		return NB_ERR_INVAL;

	int rc = orm_scope_begin(db, &lease);

	if (rc != 0)
		return rc;

	rc = orm_savepoint_begin(&lease, "history_migrate");
	if (rc != NB_OK)
		return rc;

	int count = model_history_count(db, session_id, 0);

	if (count != 0)
		return orm_savepoint_finish(&lease, "history_migrate", count < 0 ? count : 0);

	const struct norm_table_ref source = norm_table(&message_model, NB_STR_LIT("m"));
	const struct norm_table_ref existing = norm_table(&history_model, NB_STR_LIT("h"));
	const struct norm_query_field any_fields[] = {
		{.value = norm_term_value(norm_i64(1)), .into = 0}};
	const struct norm_query any = {
		.from = &existing,
		.projection = &orm_integer_model,
		.fields = any_fields,
		.field_count = 1,
		.where = norm_eq(norm_table_col(&existing, HIST_SESSION_ID), norm_i64(session_id))};
	const struct norm_query_order order[] = {
		{.value = norm_term_column(norm_table_col(&source, MESSAGE_CREATED_AT)),
		 .direction = NORM_ASC},
		{.value = norm_term_column(norm_table_col(&source, MESSAGE_ID)),
		 .direction = NORM_ASC}};
	const struct norm_term user = norm_term_value(orm_string("user_message"));
	const struct norm_term assistant = norm_term_value(orm_string("assistant_message"));
	const struct norm_case kinds[] = {
		{.when = norm_eq(norm_table_col(&source, MESSAGE_ROLE), orm_string("user")),
		 .then = &user}};
	const struct norm_term prefix = norm_term_value(orm_string("legacy:message:"));
	const struct norm_term id = norm_term_column(norm_table_col(&source, MESSAGE_ID));
	const size_t selected[] = {
		HIST_SESSION_ID, HIST_SEQUENCE_NO, HIST_TURN_ID,	 HIST_KIND,
		HIST_ROLE,	 HIST_CONTENT,	   HIST_IDEMPOTENCY_KEY, HIST_TOKEN_COUNT,
		HIST_TRUNCATED,	 HIST_ACTIVE,	   HIST_CREATED_AT};
	struct norm_field fields[11] = {0};
	for (size_t i = 0; i < 11; i++)
		fields[i] = history_model.fields[selected[i]];

	const struct norm_model projection = {.size = sizeof(struct history_row),
					      .alignment = _Alignof(struct history_row),
					      .fields = fields,
					      .field_count = 11};
	const struct norm_query_field columns[] = {
		{.value = norm_term_column(norm_table_col(&source, MESSAGE_SESSION_ID)), .into = 0},
		{.value = norm_term_row_number(
			 (struct norm_window){.order = order, .order_count = 2}),
		 .into = 1},
		{.value = norm_term_column(norm_table_col(&source, MESSAGE_TURN_ID)), .into = 2},
		{.value = norm_term_case(kinds, 1, &assistant), .into = 3},
		{.value = norm_term_column(norm_table_col(&source, MESSAGE_ROLE)), .into = 4},
		{.value = norm_term_column(norm_table_col(&source, MESSAGE_CONTENT)), .into = 5},
		{.value = norm_term_binary(NORM_BINARY_CONCAT, &prefix, &id), .into = 6},
		{.value = norm_term_column(norm_table_col(&source, MESSAGE_TOKEN_COUNT)),
		 .into = 7},
		{.value = norm_term_value(norm_i32(0)), .into = 8},
		{.value = norm_term_value(norm_i32(1)), .into = 9},
		{.value = norm_term_column(norm_table_col(&source, MESSAGE_CREATED_AT)),
		 .into = 10},
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&source, MESSAGE_SESSION_ID), norm_i64(session_id)),
		norm_not(&(struct norm_expr){.kind = NORM_EXPR_EXISTS, .as.query = &any})};
	const struct norm_query query = {.from = &source,
					 .projection = &projection,
					 .fields = columns,
					 .field_count = 11,
					 .where = norm_and(conditions, 2),
					 .order = order,
					 .order_count = 2};
	struct norm_error error = {0};

	rc = orm_status(db,
			norm_insert_select(lease.connection, &history_model, selected, 11, &query,
					   NULL, orm_scope_call(&lease), NULL, &error),
			&error);
	return orm_savepoint_finish(&lease, "history_migrate", rc);
}

static int64_t *history_preserved_add(nb_vec *preserved, int64_t id)
{
	for (size_t i = 0; i < preserved->len; i++) {
		int64_t *existing = nb_vec_at(preserved, i);

		if (*existing == id)
			return existing;
	}

	int64_t *slot = nb_vec_push(preserved);

	if (slot)
		*slot = id;

	return slot;
}

/* Selected rows share budgets even when several policies select the same row. */
static int history_preserved_contains(const nb_vec *preserved, int64_t id)
{
	for (size_t i = 0; i < preserved->len; i++) {
		const int64_t *existing = nb_vec_at(preserved, i);

		if (*existing == id)
			return 1;
	}

	return 0;
}

struct history_pair_row {
	int64_t user_id;
	int user_tokens;
	int64_t answer_id;
	bool answer_missing;
	int answer_tokens;
	bool answer_tokens_missing;
};
static const struct norm_field pair_fields[] = {
	NORM_FIELD(struct history_pair_row, user_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct history_pair_row, user_tokens, NORM_TYPE_I32, 0),
	NORM_NULLABLE_FIELD(struct history_pair_row, answer_id, answer_missing, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct history_pair_row, answer_tokens, answer_tokens_missing,
			    NORM_TYPE_I32, 0),
};
static const struct norm_model pair_model = NORM_MODEL(struct history_pair_row, "", pair_fields);

static const struct norm_field cost_fields[] = {
	NORM_FIELD(struct history_row, id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct history_row, token_count, NORM_TYPE_I32, 0),
};
static const struct norm_model cost_model = NORM_MODEL(struct history_row, "", cost_fields);
static int history_preserve_user_pairs(struct db *db, int64_t session_id, nb_vec *preserved,
				       int *budget)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const struct norm_table_ref users = norm_table(&history_model, NB_STR_LIT("u"));
	const struct norm_table_ref answers = norm_table(&history_model, NB_STR_LIT("a"));
	const struct norm_table_ref candidates = norm_table(&history_model, NB_STR_LIT("c"));
	const struct norm_term candidate_sequence =
		norm_term_column(norm_table_col(&candidates, HIST_SEQUENCE_NO));
	const struct norm_term user_sequence =
		norm_term_column(norm_table_col(&users, HIST_SEQUENCE_NO));
	const struct norm_expr eligible[] = {
		norm_columns_eq(norm_table_col(&candidates, HIST_SESSION_ID),
				norm_table_col(&users, HIST_SESSION_ID)),
		norm_columns_eq(norm_table_col(&candidates, HIST_TURN_ID),
				norm_table_col(&users, HIST_TURN_ID)),
		norm_eq(norm_table_col(&candidates, HIST_KIND), orm_string("assistant_message")),
		norm_compare_terms(&candidate_sequence, NORM_COMPARE_GT, &user_sequence),
	};
	const struct norm_query_field candidate_fields[] = {
		{.value = norm_term_column(norm_table_col(&candidates, HIST_ID)), .into = 0}};
	const struct norm_query_order candidate_order = {.value = candidate_sequence,
							 .direction = NORM_DESC};
	const struct norm_query candidate = {.from = &candidates,
					     .projection = &orm_integer_model,
					     .fields = candidate_fields,
					     .field_count = 1,
					     .where = norm_and(eligible, 4),
					     .order = &candidate_order,
					     .order_count = 1,
					     .has_limit = true,
					     .limit = 1};
	const struct norm_term selected = norm_term_scalar(&candidate);
	const struct norm_term answer_id = norm_term_column(norm_table_col(&answers, HIST_ID));
	const struct norm_join join = {
		.table = &answers,
		.kind = NORM_JOIN_LEFT,
		.on = norm_compare_terms(&answer_id, NORM_COMPARE_EQ, &selected)};
	const struct norm_term user_tokens[] = {
		norm_term_column(norm_table_col(&users, HIST_TOKEN_COUNT)),
		norm_term_value(norm_i32(0))};
	const struct norm_term answer_tokens[] = {
		norm_term_column(norm_table_col(&answers, HIST_TOKEN_COUNT)),
		norm_term_value(norm_i32(0))};
	const struct norm_query_field fields[] = {
		{.value = norm_term_column(norm_table_col(&users, HIST_ID)), .into = 0},
		{.value = norm_term_call(NORM_FUNCTION_GREATEST, user_tokens, 2), .into = 1},
		{.value = answer_id, .into = 2},
		{.value = norm_term_call(NORM_FUNCTION_GREATEST, answer_tokens, 2), .into = 3},
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&users, HIST_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_table_col(&users, HIST_ACTIVE), norm_i32(1)),
		norm_eq(norm_table_col(&users, HIST_KIND), orm_string("user_message"))};
	const struct norm_query_order order = {.value = user_sequence, .direction = NORM_DESC};
	const struct norm_result_options limits = {.max_rows = SIZE_MAX, .max_bytes = SIZE_MAX};
	const struct norm_query query = {.from = &users,
					 .joins = &join,
					 .join_count = 1,
					 .projection = &pair_model,
					 .fields = fields,
					 .field_count = 4,
					 .where = norm_and(conditions, 3),
					 .order = &order,
					 .order_count = 1,
					 .result_options = &limits};
	int rc = orm_scope_begin(db, &lease);

	if (rc == 0)
		rc = orm_status(db,
				norm_query_select(lease.connection, &query, orm_scope_call(&lease),
						  &rows, &error),
				&error);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct history_pair_row *row = norm_rows_at(rows, i);
		int64_t cost = 0;

		if (!history_preserved_contains(preserved, row->user_id))
			cost += row->user_tokens;

		if (row->answer_id && !history_preserved_contains(preserved, row->answer_id))
			cost += row->answer_tokens;

		if (cost < 0)
			cost = 0;

		if (cost > *budget)
			continue;

		if (!history_preserved_add(preserved, row->user_id) ||
		    (row->answer_id && !history_preserved_add(preserved, row->answer_id)))
			return NB_ERR_NOMEM;

		*budget -= (int)cost;
	}

	return rc;
}

static int history_preserve_rounds(struct db *db, int64_t session_id, nb_vec *preserved, int rounds,
				   int *budget)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *turns NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const struct norm_table_ref source = norm_table(&history_model, NB_STR_LIT("h"));
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&source, HIST_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_table_col(&source, HIST_ACTIVE), norm_i32(1)),
		norm_is_not_null(norm_table_col(&source, HIST_TURN_ID)),
		norm_ne(norm_table_col(&source, HIST_TURN_ID), orm_string("")),
		norm_ne(norm_table_col(&source, HIST_KIND), orm_string("compaction_summary"))};
	const struct norm_term turn_id = norm_term_column(norm_table_col(&source, HIST_TURN_ID));
	const struct norm_term sequence =
		norm_term_column(norm_table_col(&source, HIST_SEQUENCE_NO));
	const struct norm_query_field fields[] = {{.value = turn_id, .into = 0}};
	const struct norm_query_order order = {
		.value = norm_term_call(NORM_FUNCTION_MAX, &sequence, 1), .direction = NORM_DESC};
	const struct norm_query query = {.from = &source,
					 .projection = &orm_text_model,
					 .fields = fields,
					 .field_count = 1,
					 .where = norm_and(conditions, 5),
					 .group = &turn_id,
					 .group_count = 1,
					 .order = &order,
					 .order_count = 1,
					 .has_limit = true,
					 .limit = (uint64_t)rounds};
	int rc = orm_scope_begin(db, &lease);

	if (rc == 0)
		rc = orm_status(db,
				norm_query_select(lease.connection, &query, orm_scope_call(&lease),
						  &turns, &error),
				&error);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(turns); i++) {
		const struct orm_text *turn = norm_rows_at(turns, i);
		struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
		const struct norm_expr visible[] = {
			norm_eq(norm_table_col(&source, HIST_ACTIVE), norm_i32(1)),
			norm_eq(norm_table_col(&source, HIST_KIND),
				orm_string("assistant_message"))};
		const struct norm_expr selection[] = {
			norm_eq(norm_table_col(&source, HIST_SESSION_ID), norm_i64(session_id)),
			norm_eq(norm_table_col(&source, HIST_TURN_ID), norm_text(turn->value)),
			norm_ne(norm_table_col(&source, HIST_KIND),
				orm_string("compaction_summary")),
			norm_or(visible, 2)};
		const struct norm_term costs[] = {
			norm_term_column(norm_table_col(&source, HIST_TOKEN_COUNT)),
			norm_term_value(norm_i32(0))};
		const struct norm_query_field cost_fields_query[] = {
			{.value = norm_term_column(norm_table_col(&source, HIST_ID)), .into = 0},
			{.value = norm_term_call(NORM_FUNCTION_GREATEST, costs, 2), .into = 1}};
		const struct norm_result_options limits = {.max_rows = SIZE_MAX,
							   .max_bytes = SIZE_MAX};
		const struct norm_query selected = {.from = &source,
						    .projection = &cost_model,
						    .fields = cost_fields_query,
						    .field_count = 2,
						    .where = norm_and(selection, 4),
						    .result_options = &limits};
		int64_t cost = 0;

		rc = orm_status(db,
				norm_query_select(lease.connection, &selected,
						  orm_scope_call(&lease), &rows, &error),
				&error);
		if (rc != 0)
			return rc;

		for (size_t j = 0; j < norm_rows_count(rows); j++) {
			const struct history_row *row = norm_rows_at(rows, j);

			if (!history_preserved_contains(preserved, row->id))
				cost += row->token_count;
		}

		if (cost > *budget)
			break;

		for (size_t j = 0; j < norm_rows_count(rows); j++) {
			const struct history_row *row = norm_rows_at(rows, j);

			if (!history_preserved_add(preserved, row->id))
				return NB_ERR_NOMEM;
		}

		*budget -= (int)cost;
	}

	return rc;
}

static int history_select_retained_apply(struct db *db, int64_t session_id, int user_message_tokens,
					 int recent_history_tokens, int keep_recent_rounds,
					 nb_vec *preserved)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	int rc = orm_scope_begin(db, &lease);

	if (rc == 0)
		rc = history_preserve_user_pairs(db, session_id, preserved, &user_message_tokens);

	if (rc == 0) {
		recent_history_tokens = user_message_tokens > INT_MAX - recent_history_tokens
						? INT_MAX
						: recent_history_tokens + user_message_tokens;
		rc = history_preserve_rounds(db, session_id, preserved, keep_recent_rounds,
					     &recent_history_tokens);
	}

	if (rc != 0 || recent_history_tokens <= 0)
		return rc;

	struct norm_error error = {0};
	const struct norm_value excluded[] = {orm_string("user_message"),
					      orm_string("compaction_summary")};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&history_model, HIST_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_col(&history_model, HIST_ACTIVE), norm_i32(1)),
		norm_not_in(norm_col(&history_model, HIST_KIND), excluded, 2)};
	const size_t fields[] = {HIST_ID, HIST_KIND, HIST_TOKEN_COUNT};
	const struct norm_order order = {.column = norm_col(&history_model, HIST_SEQUENCE_NO),
					 .direction = NORM_DESC};
	const struct norm_select_query query = {.where = norm_and(conditions, 3),
						.fields = fields,
						.field_count = 3,
						.order = &order,
						.order_count = 1,
						.max_rows = SIZE_MAX,
						.max_bytes = SIZE_MAX};

	rc = orm_status(db,
			norm_select(lease.connection, &history_model, &query,
				    orm_scope_call(&lease), &rows, &error),
			&error);
	int need_tool_call = 0;
	size_t group_start = preserved->len;
	int recent_used_tokens = 0;

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct history_row *row = norm_rows_at(rows, i);
		int tokens = row->token_count < 0 ? 0 : row->token_count;

		if (history_preserved_contains(preserved, row->id))
			continue;

		if (tokens > recent_history_tokens - recent_used_tokens) {
			preserved->len = group_start;
			break;
		}

		if (!history_preserved_add(preserved, row->id))
			return NB_ERR_NOMEM;

		recent_used_tokens += tokens;
		if (nb_str_eq_cstr(row->kind, "tool_result"))
			need_tool_call = 1;
		else if (need_tool_call && nb_str_eq_cstr(row->kind, "assistant_tool_calls"))
			need_tool_call = 0;

		if (!need_tool_call)
			group_start = preserved->len;

		if (!need_tool_call && recent_used_tokens >= recent_history_tokens)
			break;
	}

	if (need_tool_call)
		preserved->len = group_start;

	return rc;
}

int model_history_select_retained(struct db *db, int64_t session_id, int user_message_tokens,
				  int recent_history_tokens, int keep_recent_rounds,
				  nb_vec *preserved)
{
	if (!db_is_open(db) || session_id <= 0 || !preserved || user_message_tokens < 0 ||
	    recent_history_tokens < 0 || keep_recent_rounds < 0)
		return NB_ERR_INVAL;

	int rc = (nb_vec_init(preserved, sizeof(int64_t)),
		  nb_vec_reserve(preserved, 8) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;

	rc = history_select_retained_apply(db, session_id, user_message_tokens,
					   recent_history_tokens, keep_recent_rounds, preserved);
	if (rc != 0)
		nb_vec_cleanup(preserved);

	return rc;
}

static int history_compact_apply(struct db *db, int64_t session_id, const char *turn_id,
				 const char *summary, int summary_tokens, int user_message_tokens,
				 int recent_history_tokens, int input_tokens,
				 int keep_recent_rounds, const char *trigger_kind,
				 int64_t *summary_item_id, const nb_vec *selected,
				 int64_t expected_cutoff, nb_vec *preserved)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct model_history_insert insert = {0};
	char compact_key[256];
	int64_t new_id = 0;
	int64_t cutoff_sequence_no = 0;
	int rc;

	if (selected) {
		int64_t *ids = nb_vec_push_n(preserved, selected->len);
		if (selected->len && !ids) {
			return NB_ERR_NOMEM;
		}

		if (selected->len)
			memcpy(ids, selected->data, selected->len * sizeof(*ids));
	} else {
		nb_vec_cleanup(preserved);
		rc = model_history_select_retained(db, session_id, user_message_tokens,
						   recent_history_tokens, keep_recent_rounds,
						   preserved);
		if (rc != 0) {
			return rc;
		}
	}

	rc = orm_scope_begin(db, &lease);
	struct norm_error error = {0};
	const struct norm_table_ref source = norm_table(&history_model, NB_STR_LIT("h"));
	const struct norm_term sequence =
		norm_term_column(norm_table_col(&source, HIST_SEQUENCE_NO));
	const struct norm_term defaults[] = {norm_term_call(NORM_FUNCTION_MAX, &sequence, 1),
					     norm_term_value(norm_i64(0))};
	const struct norm_query_field fields[] = {
		{.value = norm_term_call(NORM_FUNCTION_COALESCE, defaults, 2), .into = 0}};
	const struct norm_query query = {
		.from = &source,
		.projection = &orm_integer_model,
		.fields = fields,
		.field_count = 1,
		.where = norm_eq(norm_table_col(&source, HIST_SESSION_ID), norm_i64(session_id))};

	if (rc == 0)
		rc = orm_status(db,
				norm_query_select(lease.connection, &query, orm_scope_call(&lease),
						  &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return MORPH_ERR_DB;

	const struct orm_integer *last = norm_rows_at(rows, 0);

	cutoff_sequence_no = last->value;
	if (selected && cutoff_sequence_no != expected_cutoff)
		return NB_ERR_AGAIN;

	const struct norm_expr active[] = {
		norm_eq(norm_col(&history_model, HIST_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_col(&history_model, HIST_ACTIVE), norm_i32(1))};
	const struct norm_assignment deactivate[] = {
		{.column = norm_col(&history_model, HIST_ACTIVE), .value = norm_i32(0)}};
	const struct norm_update_query clear = {
		.set = deactivate, .set_count = 1, .where = norm_and(active, 2)};
	rc = orm_status(db,
			norm_update(lease.connection, &history_model, &clear,
				    orm_scope_call(&lease), NULL, &error),
			&error);
	for (size_t i = 0; rc == 0 && i < preserved->len; i++) {
		const int64_t *kept = nb_vec_at(preserved, i);
		const struct norm_assignment activate[] = {
			{.column = norm_col(&history_model, HIST_ACTIVE), .value = norm_i32(1)}};
		const struct norm_update_query restore = {
			.set = activate,
			.set_count = 1,
			.where = norm_eq(norm_col(&history_model, HIST_ID), norm_i64(*kept))};

		rc = orm_status(db,
				norm_update(lease.connection, &history_model, &restore,
					    orm_scope_call(&lease), NULL, &error),
				&error);
	}

	if (rc != 0)
		return rc;

	insert.session_id = session_id;
	insert.turn_id = turn_id;
	insert.kind = "compaction_summary";
	insert.role = "system";
	insert.content = summary;
	int length = snprintf(compact_key, sizeof(compact_key), "compaction:%s:%s:%lld",
			      turn_id ? turn_id : "unknown", trigger_kind,
			      (long long)cutoff_sequence_no);

	if (length < 0 || (size_t)length >= sizeof(compact_key))
		return NB_ERR_NAME_TOO_LONG;

	insert.idempotency_key = compact_key;
	insert.token_count = summary_tokens;
	insert.active = 1;
	rc = model_history_add(db, &insert, &new_id);
	if (rc != 0)
		return rc;

	const struct norm_model *compactions = schema_model(SCHEMA_HISTORY_COMPACTIONS);
	const char *const names[] = {"session_id",	"turn_id",	"cutoff_sequence_no",
				     "summary_item_id", "input_tokens", "output_tokens",
				     "trigger_kind",	"created_at"};
	const struct norm_value values[] = {
		norm_i64(session_id),	      orm_string(turn_id),
		norm_i64(cutoff_sequence_no), norm_i64(new_id),
		norm_i32(input_tokens),	      norm_i32(summary_tokens),
		orm_string(trigger_kind),     norm_i64((int64_t)time(NULL))};
	struct norm_field insert_fields[8] = {0};
	struct norm_query_field insert_columns[8] = {0};
	size_t targets[8] = {0};
	for (size_t i = 0; i < 8; i++) {
		targets[i] = schema_field(compactions, names[i]);
		insert_fields[i] = compactions->fields[targets[i]];
		insert_columns[i] =
			(struct norm_query_field){.value = norm_term_value(values[i]), .into = i};
	}

	const struct norm_model insert_projection = {.size = compactions->size,
						     .alignment = compactions->alignment,
						     .fields = insert_fields,
						     .field_count = 8};
	const struct norm_query inserted = {
		.projection = &insert_projection, .fields = insert_columns, .field_count = 8};
	rc = orm_status(db,
			norm_insert_select(lease.connection, compactions, targets, 8, &inserted,
					   NULL, orm_scope_call(&lease), NULL, &error),
			&error);
	if (rc == 0 && summary_item_id)
		*summary_item_id = new_id;

	return rc;
}

static int history_compact_window(struct db *db, int64_t session_id, const char *turn_id,
				  const char *summary, int summary_tokens, int user_message_tokens,
				  int recent_history_tokens, int input_tokens,
				  int keep_recent_rounds, const char *trigger_kind,
				  int64_t *summary_item_id, const nb_vec *selected,
				  int64_t expected_cutoff)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	nb_vec preserved NB_CLEANUP(nb_vec_cleanup) = {0};
	int64_t new_id = 0;

	if (!db_is_open(db) || session_id <= 0 || !summary || !summary[0] || summary_tokens < 0 ||
	    user_message_tokens < 0 || recent_history_tokens < 0 || keep_recent_rounds < 0 ||
	    !trigger_kind || !trigger_kind[0])
		return NB_ERR_INVAL;

	int rc = (nb_vec_init(&preserved, sizeof(int64_t)),
		  nb_vec_reserve(&preserved, 8) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;

	rc = orm_scope_begin(db, &lease);
	if (rc == 0)
		rc = orm_savepoint_begin(&lease, "model_history_compact");

	if (rc != 0)
		return rc;

	rc = history_compact_apply(db, session_id, turn_id, summary, summary_tokens,
				   user_message_tokens, recent_history_tokens, input_tokens,
				   keep_recent_rounds, trigger_kind, &new_id, selected,
				   expected_cutoff, &preserved);
	rc = orm_savepoint_finish(&lease, "model_history_compact", rc);
	if (rc != 0)
		return rc;

	if (summary_item_id)
		*summary_item_id = new_id;

	return 0;
}

int model_history_compact(struct db *db, int64_t session_id, const char *turn_id,
			  const char *summary, int summary_tokens, int user_message_tokens,
			  int recent_history_tokens, int input_tokens, int keep_recent_rounds,
			  const char *trigger_kind, int64_t *summary_item_id)
{
	return history_compact_window(db, session_id, turn_id, summary, summary_tokens,
				      user_message_tokens, recent_history_tokens, input_tokens,
				      keep_recent_rounds, trigger_kind, summary_item_id, NULL, 0);
}

int model_history_compact_selected(struct db *db, int64_t session_id, const char *turn_id,
				   const char *summary, int summary_tokens, int input_tokens,
				   const char *trigger_kind, int64_t *summary_item_id,
				   const nb_vec *preserved, int64_t cutoff)
{
	if (!preserved)
		return NB_ERR_INVAL;

	return history_compact_window(db, session_id, turn_id, summary, summary_tokens, 0, 0,
				      input_tokens, 0, trigger_kind, summary_item_id, preserved,
				      cutoff);
}
