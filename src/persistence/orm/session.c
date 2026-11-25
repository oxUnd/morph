#include "session.h"
#include "session_models.h"
#include <norm/query.h>
#include "errors.h"
#include <nb/log.h>
#include <nb/random.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SESSION_DISPLAY_ID_RANDOM_BYTES 4

struct trace_row {
	int64_t id;
	int64_t session_id;
	int32_t round_no;
	nb_str steps_json;
	int32_t aborted;
	bool aborted_missing;
	int64_t created_at;
};

static const struct norm_field trace_fields[] = {
	NORM_FIELD(struct trace_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_FIELD(struct trace_row, session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct trace_row, round_no, NORM_TYPE_I32, 0),
	NORM_TEXT_FIELD(struct trace_row, steps_json, 0, 0),
	NORM_NULLABLE_FIELD(struct trace_row, aborted, aborted_missing, NORM_TYPE_I32, 0),
	NORM_FIELD(struct trace_row, created_at, NORM_TYPE_I64, 0),
};

static const struct norm_model trace_model =
	NORM_MODEL(struct trace_row, "react_traces", trace_fields);

struct session_child_row {
	int64_t parent_session_id;
	bool parent_session_id_missing;
	int64_t child_session_id;
	bool child_session_id_missing;
};

static const struct norm_field session_child_fields[] = {
	NORM_NULLABLE_FIELD(struct session_child_row, child_session_id, child_session_id_missing,
			    NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct session_child_row, parent_session_id, parent_session_id_missing,
			    NORM_TYPE_I64, 0),
};
static const struct norm_model session_child_model =
	NORM_MODEL(struct session_child_row, "sub_agent_tasks", session_child_fields);

struct session_agent_row {
	int64_t root_session_id;
	int64_t session_id;
};

static const struct norm_field session_agent_fields[] = {
	NORM_FIELD(struct session_agent_row, session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct session_agent_row, root_session_id, NORM_TYPE_I64, 0),
};
static const struct norm_model session_agent_model =
	NORM_MODEL(struct session_agent_row, "agent_threads", session_agent_fields);

static void session_array_cleanup(struct session **owner)
{
	free(*owner);
	*owner = NULL;
}

static int generate_display_id(char *buffer, size_t capacity)
{
	nb_buf text NB_CLEANUP(nb_buf_destroy) = {0};
	int rc = nb_random_hex(&text, SESSION_DISPLAY_ID_RANDOM_BYTES, NULL);

	return rc == 0 ? nb_str_copy_to(nb_buf_str(&text), buffer, capacity) : rc;
}

static int session_read_row(const struct session_row *row, struct session *session)
{
	struct session value = {
		.id = row->id,
		.created_at = row->created_at,
		.updated_at = row->updated_at,
		.token_used = row->token_used,
	};
	int rc = nb_str_copy_to(row->display_id_missing ? NB_STR_LIT("") : row->display_id,
				value.display_id, sizeof(value.display_id));

	if (rc == 0)
		rc = nb_str_copy_to(row->name, value.name, sizeof(value.name));

	if (rc == 0)
		rc = nb_str_copy_to(row->model_missing ? NB_STR_LIT("") : row->model, value.model,
				    sizeof(value.model));

	if (rc == 0)
		rc = nb_str_copy_to(row->workdir, value.workdir, sizeof(value.workdir));

	if (rc == 0)
		*session = value;

	return rc;
}

int session_create_with_workdir(struct db *db, const char *name, const char *model,
				const char *workdir, struct session *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	struct norm_write_result result = {0};
	struct session value = {0};

	if (!db_is_open(db) || !name)
		return NB_ERR_INVAL;

	int rc = nb_str_copy_to(nb_str_from_cstr(name), value.name, sizeof(value.name));

	if (rc == 0)
		rc = nb_str_copy_to(nb_str_from_cstr(model ? model : ""), value.model,
				    sizeof(value.model));

	if (rc == 0)
		rc = nb_str_copy_to(nb_str_from_cstr(workdir ? workdir : ""), value.workdir,
				    sizeof(value.workdir));

	if (rc == 0)
		rc = orm_scope_begin(db, &scope);

	if (rc != 0)
		return rc;

	value.created_at = (int64_t)time(NULL);
	value.updated_at = value.created_at;
	for (int attempt = 0; attempt < 4; attempt++) {
		rc = generate_display_id(value.display_id, sizeof(value.display_id));
		if (rc != 0)
			return rc;

		struct session_row row = {
			.display_id = nb_str_from_cstr(value.display_id),
			.name = nb_str_from_cstr(value.name),
			.model = nb_str_from_cstr(value.model),
			.workdir = nb_str_from_cstr(value.workdir),
			.created_at = value.created_at,
			.updated_at = value.updated_at,
		};
		nb_status status = norm_insert(scope.connection, &session_model, &row, NULL, NULL,
					       &result, &error);

		if (status == NORM_ERR_UNIQUE || status == NORM_ERR_CONSTRAINT) {
			rc = NB_ERR_EXISTS;
			continue;
		}

		rc = orm_status(db, status, &error);
		break;
	}

	if (rc != 0)
		return rc;

	if (!result.has_insert_id)
		return MORPH_ERR_DB;

	value.id = result.insert_id;
	if (out)
		*out = value;

	nb_log_write(db->logger, NB_LOG_INFO, "session created: %s (id=%lld, display=%s)",
		     value.name, (long long)value.id, value.display_id);
	return 0;
}

int session_create(struct db *db, const char *name, const char *model, struct session *out)
{
	return session_create_with_workdir(db, name, model, NULL, out);
}

static int session_get(const struct db *db, enum session_field field, struct norm_value value,
		       struct session *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	struct norm_select_query query = {
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
		.where = norm_eq(norm_col(&session_model, (size_t)field), value)};

	if (!db_is_open(db) || !out)
		return NB_ERR_INVAL;

	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(
			db,
			norm_select(scope.connection, &session_model, &query, NULL, &rows, &error),
			&error);

	if (rc != 0)
		return rc;

	return norm_rows_count(rows) ? session_read_row(norm_rows_at(rows, 0), out)
				     : NB_ERR_NOT_FOUND;
}

int session_get_by_name(struct db *db, const char *name, struct session *out)
{
	return name ? session_get(db, SESSION_NAME, orm_string(name), out) : NB_ERR_INVAL;
}

int session_get_by_id(const struct db *db, int64_t id, struct session *out)
{
	return session_get(db, SESSION_ID, norm_i64(id), out);
}

int session_get_by_display_id(struct db *db, const char *display_id, struct session *out)
{
	return display_id ? session_get(db, SESSION_DISPLAY_ID, orm_string(display_id), out)
			  : NB_ERR_INVAL;
}

int session_list(struct db *db, struct session **out, int *count, int limit, const char *filter)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	nb_buf pattern NB_CLEANUP(nb_buf_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_table_ref sessions_ref = norm_table(&session_model, NB_STR_LIT("s"));
	const struct norm_table_ref children_ref =
		norm_table(&session_child_model, NB_STR_LIT("c"));
	const struct norm_table_ref agents_ref = norm_table(&session_agent_model, NB_STR_LIT("a"));
	const struct norm_join joins[] = {
		{.kind = NORM_JOIN_LEFT,
		 .table = &children_ref,
		 .on = norm_columns_eq(norm_table_col(&sessions_ref, SESSION_ID),
				       norm_table_col(&children_ref, 0))},
		{.kind = NORM_JOIN_LEFT,
		 .table = &agents_ref,
		 .on = norm_columns_eq(norm_table_col(&sessions_ref, SESSION_ID),
				       norm_table_col(&agents_ref, 0))},
	};
	struct norm_expr conditions[] = {
		norm_is_null(norm_table_col(&children_ref, 0)),
		norm_is_null(norm_table_col(&agents_ref, 0)),
		{0},
	};
	struct norm_join_column columns[8] = {0};
	struct norm_order order = {.column = norm_table_col(&sessions_ref, SESSION_UPDATED_AT),
				   .direction = NORM_DESC};

	if (!db_is_open(db) || !out || !count)
		return NB_ERR_INVAL;

	*out = NULL;
	*count = 0;
	size_t condition_count = 2;
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0 && filter && filter[0]) {
		(void)nb_buf_append_char(&pattern, '%');
		for (const char *part = filter; *part; part++) {
			if (*part == '!')
				(void)nb_buf_append_char(&pattern, '!');

			(void)nb_buf_append_char(&pattern, *part);
		}

		(void)nb_buf_append_char(&pattern, '%');
		rc = nb_buf_status(&pattern);
		conditions[condition_count++] = norm_like(
			norm_table_col(&sessions_ref, SESSION_NAME), nb_buf_str(&pattern));
	}

	for (size_t i = 0; i < NB_ARRAY_SIZE(columns); i++)
		columns[i] = (struct norm_join_column){norm_table_col(&sessions_ref, i), i};

	const struct norm_join_query query = {
		.from = &sessions_ref,
		.joins = joins,
		.join_count = NB_ARRAY_SIZE(joins),
		.projection = &session_model,
		.columns = columns,
		.column_count = NB_ARRAY_SIZE(columns),
		.select =
			{
				.where = norm_and(conditions, condition_count),
				.order = &order,
				.order_count = 1,
				.has_limit = limit > 0,
				.limit = limit > 0 ? (uint64_t)limit : 0,
				.max_rows = SIZE_MAX,
				.max_bytes = SIZE_MAX,
			},
	};

	if (rc == 0)
		rc = orm_status(db, norm_join_select(scope.connection, &query, NULL, &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	size_t length = norm_rows_count(rows);

	if (length > INT_MAX || length > SIZE_MAX / sizeof(struct session))
		return NB_ERR_RANGE;

	struct session *sessions NB_CLEANUP(session_array_cleanup) = NULL;

	sessions = length ? calloc(length, sizeof(*sessions)) : NULL;
	if (length && !sessions)
		return NB_ERR_NOMEM;

	for (size_t i = 0; rc == 0 && i < length; i++)
		rc = session_read_row(norm_rows_at(rows, i), &sessions[i]);

	if (rc == 0) {
		*out = sessions;
		sessions = NULL;
		*count = (int)length;
	}

	return rc;
}

static int session_integer(const struct db *db, const struct norm_query *query)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(db, norm_query_select(scope.connection, query, NULL, &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return MORPH_ERR_DB;

	const struct orm_integer *row = norm_rows_at(rows, 0);

	return row->value < INT_MIN || row->value > INT_MAX ? NB_ERR_RANGE : (int)row->value;
}

int session_count(struct db *db)
{
	const struct norm_table_ref sessions = norm_table(&session_model, NB_STR_LIT("s"));
	const struct norm_query_field count = {norm_term_call(NORM_FUNCTION_COUNT, NULL, 0), 0};
	const struct norm_query query = {.from = &sessions,
					 .projection = &orm_integer_model,
					 .fields = &count,
					 .field_count = 1};

	return session_integer(db, &query);
}

static int session_assign(struct db *db, int64_t id, const struct norm_assignment *assignments,
			  size_t count, int require_existing)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	struct norm_write_result result = {0};
	struct norm_update_query query = {
		.set = assignments,
		.set_count = count,
		.where = norm_eq(norm_col(&session_model, SESSION_ID), norm_i64(id)),
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(db,
				norm_update(scope.connection, &session_model, &query, NULL, &result,
					    &error),
				&error);

	return rc == 0 && require_existing && result.affected_rows_known && !result.affected_rows
		       ? NB_ERR_NOT_FOUND
		       : rc;
}

int session_rename(struct db *db, int64_t id, const char *new_name)
{
	if (!new_name)
		return NB_ERR_INVAL;

	const struct norm_assignment assignments[] = {
		{.column = norm_col(&session_model, SESSION_NAME), .value = orm_string(new_name)},
		{.column = norm_col(&session_model, SESSION_UPDATED_AT),
		 .value = norm_i64((int64_t)time(NULL))},
	};

	return session_assign(db, id, assignments, 2, 1);
}

int session_delete(struct db *db, int64_t id)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_table_ref children = norm_table(&session_child_model, NB_STR_LIT("c"));
	const struct norm_table_ref agents = norm_table(&session_agent_model, NB_STR_LIT("a"));
	const struct norm_query_field child_id = {norm_term_column(norm_table_col(&children, 0)),
						  0};
	const struct norm_query child_query = {
		.from = &children,
		.projection = &orm_integer_model,
		.fields = &child_id,
		.field_count = 1,
		.where = norm_eq(norm_table_col(&children, 1), norm_i64(id))};
	const struct norm_query_field agent_id = {norm_term_column(norm_table_col(&agents, 0)), 0};
	const struct norm_query agent_query = {
		.from = &agents,
		.projection = &orm_integer_model,
		.fields = &agent_id,
		.field_count = 1,
		.where = norm_eq(norm_table_col(&agents, 1), norm_i64(id))};
	const struct norm_term session_id = norm_term_column(norm_col(&session_model, SESSION_ID));
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&session_model, SESSION_ID), norm_i64(id)),
		norm_in_query(&session_id, &child_query, false),
		norm_in_query(&session_id, &agent_query, false),
	};
	const struct norm_delete_query query = {
		.where = norm_or(conditions, NB_ARRAY_SIZE(conditions))};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_delete(scope.connection, &session_model, &query, NULL, NULL,
					   &error),
			       &error);
}

int session_update_model(struct db *db, int64_t id, const char *model)
{
	if (!model)
		return NB_ERR_INVAL;

	const struct norm_assignment assignment = {
		.column = norm_col(&session_model, SESSION_MODEL), .value = orm_string(model)};

	return session_assign(db, id, &assignment, 1, 0);
}

int session_update_workdir(struct db *db, int64_t id, const char *workdir)
{
	if (id <= 0 || !workdir)
		return NB_ERR_INVAL;

	const struct norm_assignment assignment = {
		.column = norm_col(&session_model, SESSION_WORKDIR), .value = orm_string(workdir)};

	return session_assign(db, id, &assignment, 1, 0);
}

int session_update_tokens(struct db *db, int64_t id, int64_t added_tokens)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_term previous =
		norm_term_column(norm_col(&session_model, SESSION_TOKEN_USED));
	const struct norm_term added = norm_term_value(norm_i64(added_tokens));
	const struct norm_term total = norm_term_binary(NORM_BINARY_ADD, &previous, &added);
	const struct norm_assignment changes[] = {
		{.column = norm_col(&session_model, SESSION_TOKEN_USED), .expression = &total},
		{.column = norm_col(&session_model, SESSION_UPDATED_AT),
		 .value = norm_i64((int64_t)time(NULL))},
	};
	const struct norm_update_query query = {
		.set = changes,
		.set_count = NB_ARRAY_SIZE(changes),
		.where = norm_eq(norm_col(&session_model, SESSION_ID), norm_i64(id))};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_update(scope.connection, &session_model, &query, NULL, NULL,
					   &error),
			       &error);
}

int message_add(struct db *db, int64_t session_id, const char *role, const char *content,
		int token_count)
{
	return message_add_with_turn_id(db, session_id, role, content, token_count, NULL);
}

int message_add_with_turn_id(struct db *db, int64_t session_id, const char *role,
			     const char *content, int token_count, const char *turn_id)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};

	if (!db_is_open(db) || !role || !content)
		return NB_ERR_INVAL;

	const struct norm_table_ref existing = norm_table(&message_model, NB_STR_LIT("m"));
	const struct norm_expr matches[] = {
		norm_eq(norm_table_col(&existing, MESSAGE_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_table_col(&existing, MESSAGE_TURN_ID), orm_string(turn_id)),
		norm_eq(norm_table_col(&existing, MESSAGE_ROLE), orm_string(role)),
	};
	const struct norm_query_field one = {norm_term_value(norm_i32(1)), 0};
	const struct norm_query prior = {.from = &existing,
					 .projection = &orm_integer_model,
					 .fields = &one,
					 .field_count = 1,
					 .where = norm_and(matches, NB_ARRAY_SIZE(matches))};
	const struct norm_expr exists = norm_exists(&prior);
	const struct norm_query_field selected[] = {
		{norm_term_value(norm_i64(session_id)), MESSAGE_SESSION_ID},
		{norm_term_value(orm_string(role)), MESSAGE_ROLE},
		{norm_term_value(orm_string(content)), MESSAGE_CONTENT},
		{norm_term_value(turn_id && turn_id[0] ? orm_string(turn_id) : norm_null()),
		 MESSAGE_TURN_ID},
		{norm_term_value(norm_i32(token_count)), MESSAGE_TOKEN_COUNT},
		{norm_term_value(norm_i32(0)), MESSAGE_COMPRESSED},
		{norm_term_value(norm_i64((int64_t)time(NULL))), MESSAGE_CREATED_AT},
	};
	const size_t fields[] = {MESSAGE_SESSION_ID, MESSAGE_ROLE,	  MESSAGE_CONTENT,
				 MESSAGE_TURN_ID,    MESSAGE_TOKEN_COUNT, MESSAGE_COMPRESSED,
				 MESSAGE_CREATED_AT};
	const struct norm_query query = {.projection = &message_model,
					 .fields = selected,
					 .field_count = NB_ARRAY_SIZE(selected),
					 .where = turn_id ? norm_not(&exists)
							  : (struct norm_expr){0}};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_insert_select(scope.connection, &message_model, fields,
						  NB_ARRAY_SIZE(fields), &query, NULL, NULL, NULL,
						  &error),
			       &error);
}

static void message_owner_cleanup(struct message **owner)
{
	message_free_list(*owner);
	*owner = NULL;
}

struct message *message_list(struct db *db, int64_t session_id, int *count)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	struct message *head NB_CLEANUP(message_owner_cleanup) = NULL;
	struct message **tail = &head;
	struct norm_order order[] = {
		{.column = norm_col(&message_model, 7)},
		{.column = norm_col(&message_model, 0)},
	};
	const struct norm_result_options limits = {
		.max_rows = SIZE_MAX, .max_bytes = SIZE_MAX, .max_row_bytes = SIZE_MAX};
	struct norm_select_query query = {
		.where = norm_eq(norm_col(&message_model, 1), norm_i64(session_id)),
		.order = order,
		.order_count = 2,
		.result_options = &limits,
	};

	if (!count)
		return NULL;

	*count = 0;
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(
			db,
			norm_select(scope.connection, &message_model, &query, NULL, &rows, &error),
			&error);

	if (rc != 0 || norm_rows_count(rows) > INT_MAX)
		return NULL;

	for (size_t i = 0; i < norm_rows_count(rows); i++) {
		const struct message_row *row = norm_rows_at(rows, i);
		struct message *message = calloc(1, sizeof(*message));

		if (!message)
			return NULL;

		*tail = message;
		tail = &message->next;
		message->id = row->id;
		message->session_id = row->session_id;
		rc = nb_str_copy_to(row->role, message->role, sizeof(message->role));
		if (rc != 0)
			return NULL;

		message->content = row->content_missing ? strdup("") : orm_text_copy(row->content);
		message->turn_id = row->turn_id_missing ? NULL : orm_text_copy(row->turn_id);
		if (!message->content || (!row->turn_id_missing && !message->turn_id))
			return NULL;

		message->token_count = row->token_count;
		message->compressed = row->compressed;
		message->created_at = row->created_at;
	}

	*count = (int)norm_rows_count(rows);
	struct message *result = head;

	head = NULL;
	return result;
}

void message_free_list(struct message *head)
{
	while (head) {
		struct message *next = head->next;

		free(head->content);
		free(head->turn_id);
		free(head);
		head = next;
	}
}

int message_count(struct db *db, int64_t session_id)
{
	const struct norm_table_ref messages = norm_table(&message_model, NB_STR_LIT("m"));
	const struct norm_query_field count = {norm_term_call(NORM_FUNCTION_COUNT, NULL, 0), 0};
	const struct norm_query query = {
		.from = &messages,
		.projection = &orm_integer_model,
		.fields = &count,
		.field_count = 1,
		.where = norm_eq(norm_table_col(&messages, MESSAGE_SESSION_ID),
				 norm_i64(session_id))};

	return session_integer(db, &query);
}

int trace_save(struct db *db, int64_t session_id, int round_no, const char *steps_json, int aborted)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};

	if (!steps_json)
		return NB_ERR_INVAL;

	struct trace_row row = {.session_id = session_id,
				.round_no = round_no,
				.steps_json = nb_str_from_cstr(steps_json),
				.aborted = aborted,
				.created_at = (int64_t)time(NULL)};
	int rc = orm_scope_begin(db, &scope);

	return rc == 0 ? orm_status(db,
				    norm_insert(scope.connection, &trace_model, &row, NULL, NULL,
						NULL, &error),
				    &error)
		       : rc;
}

char *trace_load_latest(struct db *db, int64_t session_id, int *out_round_no, int *out_aborted)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	struct norm_order order = {.column = norm_col(&trace_model, 2), .direction = NORM_DESC};
	struct norm_select_query query = {
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
		.where = norm_eq(norm_col(&trace_model, 1), norm_i64(session_id)),
		.order = &order,
		.order_count = 1,
		.has_limit = true,
		.limit = 1,
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(
			db,
			norm_select(scope.connection, &trace_model, &query, NULL, &rows, &error),
			&error);

	if (rc != 0 || !norm_rows_count(rows))
		return NULL;

	const struct trace_row *row = norm_rows_at(rows, 0);

	if (out_round_no)
		*out_round_no = row->round_no;

	if (out_aborted)
		*out_aborted = row->aborted;

	return orm_text_copy(row->steps_json);
}

int session_ensure_display_id(struct db *db, struct session *session)
{
	if (!db_is_open(db) || !session)
		return NB_ERR_INVAL;

	if (session->display_id[0])
		return 0;

	char display[sizeof(session->display_id)] = {0};
	int rc = generate_display_id(display, sizeof(display));

	if (rc != 0)
		return rc;

	const struct norm_assignment assignment = {
		.column = norm_col(&session_model, SESSION_DISPLAY_ID),
		.value = orm_string(display),
	};

	rc = session_assign(db, session->id, &assignment, 1, 0);
	if (rc == 0)
		memcpy(session->display_id, display, sizeof(session->display_id));

	return rc;
}

int trace_get_next_round_no(struct db *db, int64_t session_id)
{
	const struct norm_table_ref traces = norm_table(&trace_model, NB_STR_LIT("t"));
	const struct norm_term round = norm_term_column(norm_table_col(&traces, 2));
	const struct norm_term arguments[] = {norm_term_call(NORM_FUNCTION_MAX, &round, 1),
					      norm_term_value(norm_i32(0))};
	const struct norm_term maximum =
		norm_term_call(NORM_FUNCTION_COALESCE, arguments, NB_ARRAY_SIZE(arguments));
	const struct norm_term one = norm_term_value(norm_i32(1));
	const struct norm_query_field next = {norm_term_binary(NORM_BINARY_ADD, &maximum, &one), 0};
	const struct norm_query query = {
		.from = &traces,
		.projection = &orm_integer_model,
		.fields = &next,
		.field_count = 1,
		.where = norm_eq(norm_table_col(&traces, 1), norm_i64(session_id))};
	int value = session_integer(db, &query);

	return value < 0 ? 1 : value;
}
