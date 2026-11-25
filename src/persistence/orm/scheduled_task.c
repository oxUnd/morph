#include "persistence/orm/scheduled_task.h"
#include "persistence/orm/internal.h"
#include "persistence/orm/records.h"
#include <nb/cleanup.h>
#include <nb/random.h>
#include <nb/encoding.h>
#include <nb/vec.h>
#include "errors.h"
#include <nb/log.h>
#include "cJSON.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct task_row {
	int64_t id;
	int64_t source_session_id;
	int64_t latest_session_id;
	nb_str title;
	nb_str kind;
	nb_str status;
	nb_str trigger_type;
	int64_t next_run_at;
	int32_t interval_seconds;
	int64_t timeout_at;
	int32_t attempts;
	int32_t max_attempts;
	nb_str action_type;
	nb_str payload_json;
	nb_str policy_json;
	nb_str notify_json;
	nb_str last_error;
	int64_t created_at;
	int64_t updated_at;
	nb_str run_token;
	bool source_missing;
	bool latest_missing;
	bool payload_missing;
	bool policy_missing;
	bool notify_missing;
	bool error_missing;
};

static const struct norm_field task_fields[] = {
	NORM_FIELD(struct task_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_NULLABLE_FIELD(struct task_row, source_session_id, source_missing, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct task_row, latest_session_id, latest_missing, NORM_TYPE_I64, 0),
	NORM_FIELD(struct task_row, title, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct task_row, kind, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct task_row, status, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct task_row, trigger_type, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct task_row, next_run_at, NORM_TYPE_I64, 0),
	NORM_FIELD(struct task_row, interval_seconds, NORM_TYPE_I32, 0),
	NORM_FIELD(struct task_row, timeout_at, NORM_TYPE_I64, 0),
	NORM_FIELD(struct task_row, attempts, NORM_TYPE_I32, 0),
	NORM_FIELD(struct task_row, max_attempts, NORM_TYPE_I32, 0),
	NORM_FIELD(struct task_row, action_type, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct task_row, payload_json, payload_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct task_row, policy_json, policy_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct task_row, notify_json, notify_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct task_row, last_error, error_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct task_row, created_at, NORM_TYPE_I64, 0),
	NORM_FIELD(struct task_row, updated_at, NORM_TYPE_I64, 0),
	NORM_FIELD(struct task_row, run_token, NORM_TYPE_TEXT, 0),
};
static const struct norm_model task_model =
	NORM_MODEL(struct task_row, "scheduled_tasks", task_fields);

struct notification_row {
	int64_t id;
	int64_t task_id;
	int64_t session_id;
	nb_str level;
	nb_str title;
	nb_str body;
	int64_t created_at;
	int64_t read_at;
	nb_str delivery_status;
	bool task_missing;
	bool session_missing;
};

static const struct norm_field notification_fields[] = {
	NORM_FIELD(struct notification_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_NULLABLE_FIELD(struct notification_row, task_id, task_missing, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct notification_row, session_id, session_missing, NORM_TYPE_I64, 0),
	NORM_FIELD(struct notification_row, level, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct notification_row, title, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct notification_row, body, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct notification_row, created_at, NORM_TYPE_I64, 0),
	NORM_FIELD(struct notification_row, read_at, NORM_TYPE_I64, 0),
	NORM_FIELD(struct notification_row, delivery_status, NORM_TYPE_TEXT, 0),
};
static const struct norm_model notification_model =
	NORM_MODEL(struct notification_row, "notifications", notification_fields);

static const struct norm_field task_counts_fields[] = {
	NORM_FIELD(struct scheduled_task_counts, running, NORM_TYPE_I64, 0),
	NORM_FIELD(struct scheduled_task_counts, pending, NORM_TYPE_I64, 0),
};
static const struct norm_model task_counts_model =
	NORM_MODEL(struct scheduled_task_counts, "", task_counts_fields);

struct task_batch {
	struct scheduled_task *items;
	int count;
};

static void task_array_cleanup(nb_vec *array)
{
	struct scheduled_task *task;

	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(array) && (task = (struct scheduled_task *)nb_vec_at(array, _i_));
	     _i_++)
		scheduled_task_cleanup(task);
	nb_vec_cleanup(array);
}

static void notification_array_cleanup(nb_vec *array)
{
	struct notification *notification;

	for (size_t _i_ = 0; _i_ < nb_vec_len(array) &&
			     (notification = (struct notification *)nb_vec_at(array, _i_));
	     _i_++)
		notification_cleanup(notification);
	nb_vec_cleanup(array);
}

static void task_batch_cleanup(struct task_batch *batch)
{
	scheduled_task_free_list(batch->items, batch->count);
	memset(batch, 0, sizeof(*batch));
}

/**
 * \brief Execute and consume an update while retaining its connection change count.
 * \param[in] db: Borrowed serialized SQLite connection.
 * \param[in] stmt: Owned statement, finalized on every path.
 * \return Changed row count, or a negative database error.
 */
static int task_change_count(struct db *db, const struct norm_model *model,
			     const struct norm_update_query *query)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_write_result result = {0};
	struct norm_error error = {0};
	int rc = orm_scope_begin(db, &scope);

	if (rc == NB_OK)
		rc = orm_status(db,
				norm_update(scope.connection, model, query, orm_scope_call(&scope),
					    &result, &error),
				&error);

	if (rc != NB_OK)
		return rc;

	return result.affected_rows > INT_MAX ? NB_ERR_RANGE : (int)result.affected_rows;
}

static int task_emit_event(const struct scheduled_task_event_sink *events, const char *name,
			   const char *phase, const char *message,
			   const struct scheduled_task *task, const char *status,
			   int64_t next_run_at, int attempts, int error_code, const char *reason,
			   const struct notification *notification)
{
	cJSON *data;
	int rc;

	if (!events || !events->cb)
		return 0;
	if (!name || !task)
		return NB_ERR_INVAL;
	data = cJSON_CreateObject();
	if (!data)
		return NB_ERR_NOMEM;
	if (!cJSON_AddNumberToObject(data, "task_id", (double)task->id) ||
	    !cJSON_AddNumberToObject(data, "source_session_id", (double)task->source_session_id) ||
	    !cJSON_AddNumberToObject(data, "latest_session_id", (double)task->latest_session_id) ||
	    !cJSON_AddStringToObject(data, "title", task->title) ||
	    !cJSON_AddStringToObject(data, "kind", task->kind) ||
	    !cJSON_AddStringToObject(data, "trigger_type", task->trigger_type) ||
	    !cJSON_AddStringToObject(data, "status", status ? status : task->status) ||
	    !cJSON_AddNumberToObject(data, "next_run_at", (double)next_run_at) ||
	    !cJSON_AddNumberToObject(data, "attempts", attempts) ||
	    !cJSON_AddNumberToObject(data, "max_attempts", task->max_attempts)) {
		cJSON_Delete(data);
		return NB_ERR_NOMEM;
	}
	if (task->interval_seconds > 0 &&
	    !cJSON_AddNumberToObject(data, "interval_seconds", task->interval_seconds)) {
		cJSON_Delete(data);
		return NB_ERR_NOMEM;
	}
	if (error_code != 0 && !cJSON_AddNumberToObject(data, "error_code", error_code)) {
		cJSON_Delete(data);
		return NB_ERR_NOMEM;
	}
	if (reason && !cJSON_AddStringToObject(data, "reason", reason)) {
		cJSON_Delete(data);
		return NB_ERR_NOMEM;
	}
	if (notification) {
		if (!cJSON_AddNumberToObject(data, "notification_id", (double)notification->id) ||
		    !cJSON_AddNumberToObject(data, "session_id",
					     (double)notification->session_id) ||
		    !cJSON_AddStringToObject(data, "notification_level", notification->level) ||
		    !cJSON_AddStringToObject(data, "notification_title", notification->title) ||
		    !cJSON_AddStringToObject(data, "notification_body",
					     notification->body ? notification->body : "")) {
			cJSON_Delete(data);
			return NB_ERR_NOMEM;
		}
	}
	rc = morph_event_emit_simple(events->cb, events->user_data, MORPH_EVENT_TASK, name, phase,
				     message, data);
	cJSON_Delete(data);
	return rc;
}

static int task_from_row(const struct task_row *row, struct scheduled_task *task)
{
	int rc;

	if (!row || !task)
		return NB_ERR_INVAL;

	memset(task, 0, sizeof(*task));
	task->id = row->id;
	task->source_session_id = row->source_session_id;
	task->latest_session_id = row->latest_session_id;
	task->next_run_at = row->next_run_at;
	task->interval_seconds = row->interval_seconds;
	task->timeout_at = row->timeout_at;
	task->attempts = row->attempts;
	task->max_attempts = row->max_attempts;
	task->created_at = row->created_at;
	task->updated_at = row->updated_at;
	rc = nb_str_copy_to(row->title, task->title, sizeof(task->title));
	if (rc == NB_OK)
		rc = nb_str_copy_to(row->kind, task->kind, sizeof(task->kind));

	if (rc == NB_OK)
		rc = nb_str_copy_to(row->status, task->status, sizeof(task->status));

	if (rc == NB_OK)
		rc = nb_str_copy_to(row->trigger_type, task->trigger_type,
				    sizeof(task->trigger_type));

	if (rc == NB_OK)
		rc = nb_str_copy_to(row->action_type, task->action_type, sizeof(task->action_type));

	if (rc == NB_OK)
		rc = nb_str_copy_to(row->last_error, task->last_error, sizeof(task->last_error));

	if (rc == NB_OK)
		rc = nb_str_copy_to(row->run_token, task->run_token, sizeof(task->run_token));

	if (rc != NB_OK)
		return rc == NB_ERR_RANGE ? NB_ERR_NAME_TOO_LONG : rc;

	if (!row->payload_missing)
		task->payload_json = orm_text_copy(row->payload_json);

	if (!row->policy_missing)
		task->policy_json = orm_text_copy(row->policy_json);

	if (!row->notify_missing)
		task->notify_json = orm_text_copy(row->notify_json);

	if ((!row->payload_missing && !task->payload_json) ||
	    (!row->policy_missing && !task->policy_json) ||
	    (!row->notify_missing && !task->notify_json)) {
		scheduled_task_cleanup(task);
		return NB_ERR_NOMEM;
	}

	return NB_OK;
}

static int notification_from_row(const struct notification_row *row,
				 struct notification *notification)
{
	int rc;

	if (!row || !notification)
		return NB_ERR_INVAL;

	memset(notification, 0, sizeof(*notification));
	notification->id = row->id;
	notification->task_id = row->task_id;
	notification->session_id = row->session_id;
	notification->created_at = row->created_at;
	notification->read_at = row->read_at;
	rc = nb_str_copy_to(row->level, notification->level, sizeof(notification->level));
	if (rc == NB_OK)
		rc = nb_str_copy_to(row->title, notification->title, sizeof(notification->title));

	if (rc == NB_OK)
		rc = nb_str_copy_to(row->delivery_status, notification->delivery_status,
				    sizeof(notification->delivery_status));

	if (rc != NB_OK)
		return rc == NB_ERR_RANGE ? NB_ERR_NAME_TOO_LONG : rc;

	notification->body = orm_text_copy(row->body);
	return notification->body ? NB_OK : NB_ERR_NOMEM;
}

static int notification_get(struct db *db, int64_t id, struct notification *notification)
{
	struct notification value NB_CLEANUP(notification_cleanup) = {0};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	struct norm_select_query query = {
		.where = norm_eq(norm_col(&notification_model, 0), norm_i64(id)),
	};
	int rc;

	if (!db_is_open(db) || id <= 0 || !notification)
		return NB_ERR_INVAL;

	if (notification->body)
		return NB_ERR_EXISTS;

	rc = orm_scope_begin(db, &scope);
	if (rc == NB_OK)
		rc = orm_status(db,
				norm_select(scope.connection, &notification_model, &query,
					    orm_scope_call(&scope), &rows, &error),
				&error);

	if (rc != NB_OK)
		return rc;

	if (!norm_rows_count(rows))
		return NB_ERR_NOT_FOUND;

	rc = notification_from_row(norm_rows_at(rows, 0), &value);
	if (rc == NB_OK) {
		*notification = value;
		memset(&value, 0, sizeof(value));
	}

	return rc;
}

static int str_in_set(const char *s, const char *const *values, int count)
{
	if (!s || !*s)
		return 0;
	for (int i = 0; i < count; i++) {
		if (strcmp(s, values[i]) == 0)
			return 1;
	}
	return 0;
}

static int scheduled_task_validate_input(const struct scheduled_task_input *input)
{
	static const char *const kinds[] = {"agent", "reminder", "action", "watch"};
	static const char *const triggers[] = {"once", "interval", "cron", "event"};

	if (!input || !input->title || !input->kind || !input->trigger_type || !input->action_type)
		return NB_ERR_INVAL;
	if (!input->title[0] || !input->kind[0] || !input->trigger_type[0] ||
	    !input->action_type[0])
		return NB_ERR_INVAL;
	if (strlen(input->title) >= SCHEDULED_TASK_TEXT_MAX ||
	    strlen(input->kind) >= SCHEDULED_TASK_TYPE_MAX ||
	    strlen(input->trigger_type) >= SCHEDULED_TASK_TYPE_MAX ||
	    strlen(input->action_type) >= SCHEDULED_TASK_TYPE_MAX)
		return NB_ERR_NAME_TOO_LONG;
	if (!str_in_set(input->kind, kinds, (int)(sizeof(kinds) / sizeof(kinds[0]))))
		return NB_ERR_INVAL;
	if (!str_in_set(input->trigger_type, triggers,
			(int)(sizeof(triggers) / sizeof(triggers[0]))))
		return NB_ERR_INVAL;
	if (input->next_run_at < 0 || input->interval_seconds < 0 || input->timeout_at < 0 ||
	    input->max_attempts < 0)
		return NB_ERR_INVAL;
	if (strcmp(input->trigger_type, "interval") == 0 && input->interval_seconds <= 0)
		return NB_ERR_INVAL;
	if ((strcmp(input->kind, "agent") == 0 || strcmp(input->kind, "action") == 0 ||
	     strcmp(input->kind, "watch") == 0) &&
	    !input->payload_json)
		return NB_ERR_INVAL;
	return 0;
}

static int task_list_query(struct db *db, const struct norm_select_query *query,
			   struct scheduled_task **out, int *count)
{
	nb_vec array NB_CLEANUP(task_array_cleanup) = {0};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};

	if (!db_is_open(db) || !query || !out || !count)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	*count = 0;
	nb_vec_init(&array, sizeof(struct scheduled_task));
	int rc = orm_scope_begin(db, &scope);

	if (rc == NB_OK)
		rc = orm_status(db,
				norm_select(scope.connection, &task_model, query,
					    orm_scope_call(&scope), &rows, &error),
				&error);

	for (size_t index = 0; rc == NB_OK && index < norm_rows_count(rows); index++) {
		struct scheduled_task *task = nb_vec_push(&array);

		if (!task)
			return NB_ERR_NOMEM;

		memset(task, 0, sizeof(*task));
		rc = task_from_row(norm_rows_at(rows, index), task);
	}

	if (rc != NB_OK)
		return rc;

	if (array.len > INT_MAX)
		return NB_ERR_RANGE;

	*out = array.len ? malloc(array.len * array.elem_size) : NULL;
	if (array.len && !*out)
		return NB_ERR_NOMEM;

	if (array.len)
		memcpy(*out, array.data, array.len * array.elem_size);

	*count = (int)array.len;
	array.len = 0;
	return NB_OK;
}

int scheduled_task_create_with_events(struct db *db, const struct scheduled_task_input *input,
				      struct scheduled_task *out,
				      const struct scheduled_task_event_sink *events)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct scheduled_task task NB_CLEANUP(scheduled_task_cleanup) = {0};
	struct norm_error error = {0};
	struct norm_write_result result = {0};
	int rc;

	if (!db_is_open(db))
		return NB_ERR_INVAL;

	if (out && (out->payload_json || out->policy_json || out->notify_json))
		return NB_ERR_EXISTS;

	rc = scheduled_task_validate_input(input);
	if (rc != NB_OK)
		return rc;

	int64_t now = (int64_t)time(NULL);
	struct task_row row = {
		.source_session_id = input->source_session_id,
		.source_missing = input->source_session_id <= 0,
		.latest_missing = true,
		.title = nb_str_from_cstr(input->title),
		.kind = nb_str_from_cstr(input->kind),
		.status = NB_STR_LIT("pending"),
		.trigger_type = nb_str_from_cstr(input->trigger_type),
		.next_run_at = input->next_run_at,
		.interval_seconds = input->interval_seconds,
		.timeout_at = input->timeout_at,
		.max_attempts = input->max_attempts,
		.action_type = nb_str_from_cstr(input->action_type),
		.payload_json = nb_str_from_cstr(input->payload_json),
		.policy_json = nb_str_from_cstr(input->policy_json),
		.notify_json = nb_str_from_cstr(input->notify_json),
		.payload_missing = !input->payload_json,
		.policy_missing = !input->policy_json,
		.notify_missing = !input->notify_json,
		.error_missing = true,
		.created_at = now,
		.updated_at = now,
		.run_token = NB_STR_LIT(""),
	};

	rc = orm_scope_begin(db, &scope);
	if (rc == NB_OK)
		rc = orm_status(db,
				norm_insert(scope.connection, &task_model, &row, NULL,
					    orm_scope_call(&scope), &result, &error),
				&error);

	if (rc == NB_OK)
		rc = scheduled_task_get(db, result.insert_id, &task);

	if (rc != NB_OK)
		return rc;

	orm_scope_cleanup(&scope);
	rc = task_emit_event(events, "task.created", "end", "task created", &task, task.status,
			     task.next_run_at, task.attempts, 0, NULL, NULL);
	if (rc == NB_OK && out) {
		*out = task;
		memset(&task, 0, sizeof(task));
	}

	return rc;
}

int scheduled_task_create(struct db *db, const struct scheduled_task_input *input,
			  struct scheduled_task *out)
{
	return scheduled_task_create_with_events(db, input, out, NULL);
}

int scheduled_task_get(struct db *db, int64_t id, struct scheduled_task *out)
{
	struct scheduled_task value NB_CLEANUP(scheduled_task_cleanup) = {0};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	struct norm_select_query query = {
		.where = norm_eq(norm_col(&task_model, 0), norm_i64(id)),
	};
	int rc;

	if (!db_is_open(db) || id <= 0 || !out)
		return NB_ERR_INVAL;

	if (out->payload_json || out->policy_json || out->notify_json)
		return NB_ERR_EXISTS;

	rc = orm_scope_begin(db, &scope);
	if (rc == NB_OK)
		rc = orm_status(db,
				norm_select(scope.connection, &task_model, &query,
					    orm_scope_call(&scope), &rows, &error),
				&error);

	if (rc != NB_OK)
		return rc;

	if (!norm_rows_count(rows))
		return NB_ERR_NOT_FOUND;

	rc = task_from_row(norm_rows_at(rows, 0), &value);
	if (rc == NB_OK) {
		*out = value;
		memset(&value, 0, sizeof(value));
	}

	return rc;
}

int scheduled_task_update_with_events(struct db *db, int64_t id,
				      const struct scheduled_task_input *input,
				      struct scheduled_task *out,
				      const struct scheduled_task_event_sink *events)
{
	nb_buf token NB_CLEANUP(nb_buf_cleanup) = {0};
	struct scheduled_task task NB_CLEANUP(scheduled_task_cleanup) = {0};
	int rc;

	if (!db_is_open(db) || id <= 0)
		return NB_ERR_INVAL;

	if (out && (out->payload_json || out->policy_json || out->notify_json))
		return NB_ERR_EXISTS;

	rc = scheduled_task_validate_input(input);
	if (rc != NB_OK)
		return rc;

	if (!nb_buf_append_cstr(&token, "revision_"))
		return NB_ERR_NOMEM;

	rc = nb_random_hex(&token, 16, NULL);
	if (rc != NB_OK)
		return rc;

	const struct norm_assignment changes[] = {
		{.column = norm_col(&task_model, 3), .value = orm_string(input->title)},
		{.column = norm_col(&task_model, 4), .value = orm_string(input->kind)},
		{.column = norm_col(&task_model, 5), .value = orm_string("pending")},
		{.column = norm_col(&task_model, 6), .value = orm_string(input->trigger_type)},
		{.column = norm_col(&task_model, 7), .value = norm_i64(input->next_run_at)},
		{.column = norm_col(&task_model, 8), .value = norm_i32(input->interval_seconds)},
		{.column = norm_col(&task_model, 9), .value = norm_i64(input->timeout_at)},
		{.column = norm_col(&task_model, 11), .value = norm_i32(input->max_attempts)},
		{.column = norm_col(&task_model, 12), .value = orm_string(input->action_type)},
		{.column = norm_col(&task_model, 13), .value = orm_string(input->payload_json)},
		{.column = norm_col(&task_model, 14), .value = orm_string(input->policy_json)},
		{.column = norm_col(&task_model, 15), .value = orm_string(input->notify_json)},
		{.column = norm_col(&task_model, 10), .value = norm_i32(0)},
		{.column = norm_col(&task_model, 16), .value = norm_null()},
		{.column = norm_col(&task_model, 18), .value = norm_i64((int64_t)time(NULL))},
		{.column = norm_col(&task_model, 19), .value = norm_text(nb_buf_str(&token))},
	};
	const struct norm_update_query query = {
		.set = changes,
		.set_count = NB_ARRAY_SIZE(changes),
		.where = norm_eq(norm_col(&task_model, 0), norm_i64(id))};

	rc = task_change_count(db, &task_model, &query);
	if (rc < 0)
		return rc;

	if (!rc)
		return NB_ERR_NOT_FOUND;

	rc = scheduled_task_get(db, id, &task);
	if (rc != NB_OK)
		return rc;

	rc = task_emit_event(events, "task.updated", "end", "task updated", &task, task.status,
			     task.next_run_at, task.attempts, 0, NULL, NULL);
	if (rc == NB_OK && out) {
		*out = task;
		memset(&task, 0, sizeof(task));
	}

	return rc;
}

int scheduled_task_update(struct db *db, int64_t id, const struct scheduled_task_input *input,
			  struct scheduled_task *out)
{
	return scheduled_task_update_with_events(db, id, input, out, NULL);
}

int scheduled_task_count_active(struct db *db, struct scheduled_task_counts *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	if (!db_is_open(db) || !out)
		return NB_ERR_INVAL;

	memset(out, 0, sizeof(*out));
	const struct norm_table_ref tasks = norm_table(&task_model, NB_STR_LIT("t"));
	const struct norm_value pending_states[] = {orm_string("pending"), orm_string("waiting")};
	const struct norm_value active_states[] = {orm_string("running"), orm_string("pending"),
						   orm_string("waiting")};
	const struct norm_term one = norm_term_value(norm_i32(1));
	const struct norm_case running = {
		.when = norm_eq(norm_table_col(&tasks, 5), orm_string("running")), .then = &one};
	const struct norm_case pending = {.when = norm_in(norm_table_col(&tasks, 5), pending_states,
							  NB_ARRAY_SIZE(pending_states)),
					  .then = &one};
	const struct norm_term running_case = norm_term_case(&running, 1, NULL);
	const struct norm_term pending_case = norm_term_case(&pending, 1, NULL);
	const struct norm_query_field selected[] = {
		{norm_term_call(NORM_FUNCTION_COUNT, &running_case, 1), 0},
		{norm_term_call(NORM_FUNCTION_COUNT, &pending_case, 1), 1},
	};
	const struct norm_query query = {.from = &tasks,
					 .projection = &task_counts_model,
					 .fields = selected,
					 .field_count = NB_ARRAY_SIZE(selected),
					 .where = norm_in(norm_table_col(&tasks, 5), active_states,
							  NB_ARRAY_SIZE(active_states))};
	int rc = orm_scope_begin(db, &scope);

	if (rc == NB_OK)
		rc = orm_read(&scope, &query, &rows);

	if (rc == NB_OK) {
		const struct scheduled_task_counts *value = norm_rows_at(rows, 0);

		if (!value)
			return MORPH_ERR_DB;

		*out = *value;
	}

	return rc;
}

int scheduled_task_list(struct db *db, const char *status, int limit, struct scheduled_task **out,
			int *count)
{
	const struct norm_order order[] = {
		{.column = norm_col(&task_model, 18), .direction = NORM_DESC},
		{.column = norm_col(&task_model, 0), .direction = NORM_DESC},
	};
	const struct norm_select_query query = {
		.where = status ? norm_eq(norm_col(&task_model, 5), orm_string(status))
				: (struct norm_expr){0},
		.order = order,
		.order_count = NB_ARRAY_SIZE(order),
		.has_limit = true,
		.limit = limit > 0 ? (uint64_t)limit : 100,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};

	return task_list_query(db, &query, out, count);
}

int scheduled_task_list_due(struct db *db, int64_t now, int limit, struct scheduled_task **out,
			    int *count)
{
	if (!db_is_open(db) || now < 0 || !out || !count)
		return NB_ERR_INVAL;

	const struct norm_value states[] = {orm_string("pending"), orm_string("waiting")};
	const struct norm_expr conditions[] = {
		norm_in(norm_col(&task_model, 5), states, NB_ARRAY_SIZE(states)),
		norm_le(norm_col(&task_model, 7), norm_i64(now)),
	};
	const struct norm_order order[] = {
		{.column = norm_col(&task_model, 7)},
		{.column = norm_col(&task_model, 0)},
	};
	const struct norm_select_query query = {
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
		.order = order,
		.order_count = NB_ARRAY_SIZE(order),
		.has_limit = true,
		.limit = limit > 0 ? (uint64_t)limit : 100,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};

	return task_list_query(db, &query, out, count);
}

static int task_update_run(struct db *db, int64_t id, const char *status, int64_t next_run_at,
			   int attempts, int64_t latest_session_id, const char *last_error,
			   const char *run_token)
{
	if (!db_is_open(db) || id <= 0 || !status || next_run_at < 0 || attempts < 0)
		return NB_ERR_INVAL;

	const struct norm_term new_session = norm_term_value(norm_i64(latest_session_id));
	const struct norm_term old_session = norm_term_column(norm_col(&task_model, 2));
	const struct norm_term zero = norm_term_value(norm_i32(0));
	const struct norm_case use_latest = {
		.when = norm_compare_terms(&new_session, NORM_COMPARE_GT, &zero),
		.then = &new_session};
	const struct norm_term session = norm_term_case(&use_latest, 1, &old_session);
	const struct norm_assignment changes[] = {
		{.column = norm_col(&task_model, 5), .value = orm_string(status)},
		{.column = norm_col(&task_model, 7), .value = norm_i64(next_run_at)},
		{.column = norm_col(&task_model, 10), .value = norm_i32(attempts)},
		{.column = norm_col(&task_model, 2), .expression = &session},
		{.column = norm_col(&task_model, 16), .value = orm_string(last_error)},
		{.column = norm_col(&task_model, 18), .value = norm_i64((int64_t)time(NULL))},
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&task_model, 0), norm_i64(id)),
		norm_eq(norm_col(&task_model, 5), orm_string("running")),
		norm_eq(norm_col(&task_model, 19), orm_string(run_token)),
	};
	const struct norm_update_query query = {.set = changes,
						.set_count = NB_ARRAY_SIZE(changes),
						.where = norm_and(conditions, run_token ? 3 : 1)};
	int rc = task_change_count(db, &task_model, &query);

	if (rc < 0)
		return rc;

	return rc ? NB_OK : (run_token ? NB_ERR_STALE : NB_ERR_NOT_FOUND);
}

int scheduled_task_update_run(struct db *db, int64_t id, const char *status, int64_t next_run_at,
			      int attempts, int64_t latest_session_id, const char *last_error)
{
	return task_update_run(db, id, status, next_run_at, attempts, latest_session_id, last_error,
			       NULL);
}

int scheduled_task_recover_stale_running(struct db *db, int64_t now, int stale_after_seconds,
					 int *recovered)
{
	if (!db_is_open(db) || now < 0 || stale_after_seconds < 0)
		return NB_ERR_INVAL;

	int64_t cutoff = now > stale_after_seconds ? now - stale_after_seconds : 0;
	const struct norm_assignment changes[] = {
		{.column = norm_col(&task_model, 5), .value = orm_string("waiting")},
		{.column = norm_col(&task_model, 7), .value = norm_i64(now)},
		{.column = norm_col(&task_model, 16),
		 .value = orm_string("Recovered after interrupted background execution")},
		{.column = norm_col(&task_model, 18), .value = norm_i64(now)},
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&task_model, 5), orm_string("running")),
		norm_le(norm_col(&task_model, 18), norm_i64(cutoff)),
	};
	const struct norm_update_query query = {
		.set = changes,
		.set_count = NB_ARRAY_SIZE(changes),
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions))};
	int rc = task_change_count(db, &task_model, &query);
	if (rc < 0)
		return rc;

	if (recovered)
		*recovered = rc;

	return NB_OK;
}

int scheduled_task_cancel_with_events(struct db *db, int64_t id,
				      const struct scheduled_task_event_sink *events)
{
	struct scheduled_task task NB_CLEANUP(scheduled_task_cleanup) = {0};

	if (!db_is_open(db) || id <= 0)
		return NB_ERR_INVAL;

	const struct norm_assignment changes[] = {
		{.column = norm_col(&task_model, 5), .value = orm_string("cancelled")},
		{.column = norm_col(&task_model, 18), .value = norm_i64((int64_t)time(NULL))},
	};
	const struct norm_update_query query = {
		.set = changes,
		.set_count = NB_ARRAY_SIZE(changes),
		.where = norm_eq(norm_col(&task_model, 0), norm_i64(id))};
	int rc = task_change_count(db, &task_model, &query);
	if (rc < 0)
		return rc;

	if (!rc)
		return NB_ERR_NOT_FOUND;

	rc = scheduled_task_get(db, id, &task);
	if (rc != NB_OK)
		return rc;

	return task_emit_event(events, "task.cancelled", "cancelled", "task cancelled", &task,
			       task.status, task.next_run_at, task.attempts, NB_ERR_CANCELLED,
			       "cancelled", NULL);
}

int scheduled_task_cancel(struct db *db, int64_t id)
{
	return scheduled_task_cancel_with_events(db, id, NULL);
}

static char *task_payload_message(const struct scheduled_task *task)
{
	cJSON *root;
	cJSON *message;
	char *out = NULL;

	if (!task || !task->payload_json)
		return NULL;
	root = cJSON_Parse(task->payload_json);
	if (!root)
		return NULL;
	message = cJSON_GetObjectItem(root, "message");
	if (cJSON_IsString(message) && message->valuestring)
		out = strdup(message->valuestring);
	cJSON_Delete(root);
	return out;
}

static int task_claim_due(struct db *db, struct scheduled_task *task)
{
	nb_buf token NB_CLEANUP(nb_buf_cleanup) = {0};
	char previous[64];
	int rc;

	if (!db_is_open(db) || !task)
		return NB_ERR_INVAL;

	memcpy(previous, task->run_token, sizeof(previous));
	if (!nb_buf_append_cstr(&token, "run_"))
		return NB_ERR_NOMEM;

	rc = nb_random_hex(&token, 16, NULL);
	if (rc == NB_OK)
		rc = nb_str_copy_to(nb_buf_str(&token), task->run_token, sizeof(task->run_token));

	if (rc != NB_OK)
		return rc;

	const struct norm_assignment changes[] = {
		{.column = norm_col(&task_model, 5), .value = orm_string("running")},
		{.column = norm_col(&task_model, 18), .value = norm_i64((int64_t)time(NULL))},
		{.column = norm_col(&task_model, 19), .value = orm_string(task->run_token)},
	};
	const struct norm_value states[] = {orm_string("pending"), orm_string("waiting")};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&task_model, 0), norm_i64(task->id)),
		norm_in(norm_col(&task_model, 5), states, NB_ARRAY_SIZE(states)),
		norm_eq(norm_col(&task_model, 7), norm_i64(task->next_run_at)),
		norm_eq(norm_col(&task_model, 19), orm_string(previous)),
	};
	const struct norm_update_query query = {
		.set = changes,
		.set_count = NB_ARRAY_SIZE(changes),
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions))};

	rc = task_change_count(db, &task_model, &query);
	return rc < 0 ? rc : (rc > 0 ? 1 : 0);
}

static const char *task_delivery_status(const struct scheduled_task *task)
{
	cJSON *root;
	cJSON *targets;
	cJSON *target;
	int has_inbox = 0;
	int has_session = 0;

	if (!task || !task->notify_json)
		return "inbox";
	root = cJSON_Parse(task->notify_json);
	if (!root)
		return "inbox";
	targets = cJSON_GetObjectItem(root, "targets");
	if (cJSON_IsArray(targets)) {
		cJSON_ArrayForEach (target, targets) {
			if (!cJSON_IsString(target) || !target->valuestring)
				continue;
			if (strcmp(target->valuestring, "inbox") == 0)
				has_inbox = 1;
			else if (strcmp(target->valuestring, "session") == 0)
				has_session = 1;
		}
	}
	cJSON_Delete(root);
	if (has_inbox && has_session)
		return "inbox+session";
	if (has_session)
		return "session";
	return "inbox";
}

static int task_emit_notification_event(const struct scheduled_task_event_sink *events,
					const struct scheduled_task *task,
					const struct notification *notification);
static int task_emit_result_event(const struct scheduled_task_event_sink *events,
				  const struct scheduled_task *task, const char *status,
				  int64_t next_run_at, int attempts, int error_code,
				  const char *reason, const struct notification *notification);

/** \brief Commit one run and its notification atomically, before observer delivery. */
static int task_commit_result(struct db *db, const struct scheduled_task *task, int64_t session_id,
			      const char *level, const char *body, const char *status,
			      int64_t next_run_at, int attempts, const char *last_error,
			      struct notification *notification)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);

	if (rc == NB_OK)
		rc = orm_savepoint_begin(&scope, "task_result");

	if (rc != NB_OK)
		return rc;

	rc = task_update_run(db, task->id, status, next_run_at, attempts, session_id, last_error,
			     task->run_token);
	if (rc == NB_OK)
		rc = notification_create_for_session(db, task->id, session_id, level, task->title,
						     body, task_delivery_status(task),
						     notification);

	rc = orm_savepoint_finish(&scope, "task_result", rc);
	if (rc != NB_OK)
		notification_cleanup(notification);

	return rc;
}

static int run_due_reminder(struct db *db, const struct scheduled_task *task, int64_t now,
			    struct notification *notification,
			    const struct scheduled_task_event_sink *events)
{
	char *body;
	const char *status;
	const char *reason;
	int attempts;
	int64_t next_run_at = 0;
	int rc;

	body = task_payload_message(task);
	if (!body)
		body = strdup(task->title);
	if (!body)
		return NB_ERR_NOMEM;

	attempts = task->attempts + 1;
	if (task->timeout_at > 0 && now >= task->timeout_at) {
		status = "timed_out";
		reason = "timeout";
	} else if (task->max_attempts > 0 && attempts >= task->max_attempts) {
		status = "completed";
		reason = "completed";
	} else if (task->interval_seconds > 0) {
		status = "waiting";
		reason = "interval";
		next_run_at = now + task->interval_seconds;
	} else {
		status = "completed";
		reason = "completed";
	}
	rc = task_commit_result(db, task, 0, "info", body, status, next_run_at, attempts, NULL,
				notification);
	free(body);
	if (rc == 0) {
		rc = task_emit_notification_event(events, task, notification);
	}
	if (rc == 0)
		rc = task_emit_result_event(events, task, status, next_run_at, attempts,
					    strcmp(status, "timed_out") == 0 ? NB_ERR_TIMEOUT : 0,
					    reason, notification);
	return rc;
}

static int action_should_retry(const struct scheduled_task *task, int attempts)
{
	if (!task)
		return 0;
	if (task->max_attempts <= 0)
		return 0;
	if (attempts >= task->max_attempts)
		return 0;
	return task->interval_seconds > 0;
}

static int task_retry_after(const struct scheduled_task *task,
			    const struct scheduled_task_action_result *result)
{
	if (result && result->retry_after_seconds > 0)
		return result->retry_after_seconds;
	if (task && task->interval_seconds > 0)
		return task->interval_seconds;
	return 60;
}

static int run_due_unsupported(struct db *db, const struct scheduled_task *task,
			       struct notification *notification,
			       const struct scheduled_task_event_sink *events);

static int task_emit_notification_event(const struct scheduled_task_event_sink *events,
					const struct scheduled_task *task,
					const struct notification *notification)
{
	if (!notification)
		return 0;
	return task_emit_event(events, "task.notification", "end", "task notification created",
			       task, task->status, task->next_run_at, task->attempts, 0, NULL,
			       notification);
}

static int task_emit_result_event(const struct scheduled_task_event_sink *events,
				  const struct scheduled_task *task, const char *status,
				  int64_t next_run_at, int attempts, int error_code,
				  const char *reason, const struct notification *notification)
{
	const char *name = "task.updated";
	const char *phase = "end";
	const char *message = "task updated";

	if (!status)
		return NB_ERR_INVAL;
	if (strcmp(status, "completed") == 0) {
		name = "task.completed";
		message = "task completed";
	} else if (strcmp(status, "waiting") == 0) {
		name = "task.rescheduled";
		phase = "ready";
		message = "task rescheduled";
	} else if (strcmp(status, "timed_out") == 0) {
		name = "task.timed_out";
		phase = "timeout";
		message = "task timed out";
	} else if (strcmp(status, "failed") == 0 && reason && strcmp(reason, "max_attempts") == 0) {
		name = "task.max_attempts_reached";
		phase = "failed";
		message = "task reached maximum attempts";
	} else if (strcmp(status, "failed") == 0) {
		name = "task.failed";
		phase = "failed";
		message = "task failed";
	}
	return task_emit_event(events, name, phase, message, task, status, next_run_at, attempts,
			       error_code, reason, notification);
}

static int run_due_agent(struct db *db, const struct scheduled_task *task, int64_t now,
			 scheduled_task_runner_fn runner, void *runner_user_data,
			 struct notification *notification,
			 const struct scheduled_task_event_sink *events)
{
	struct scheduled_task_action_result result;
	const char *body;
	const char *level;
	const char *status;
	const char *last_error = NULL;
	const char *reason = NULL;
	int error_code = 0;
	int attempts;
	int64_t next_run_at = 0;
	int rc;
	int nrc;

	if (!db || !task || !notification)
		return NB_ERR_INVAL;
	if (!runner)
		return run_due_unsupported(db, task, notification, events);

	memset(&result, 0, sizeof(result));
	rc = runner(task, &result, runner_user_data);
	attempts = task->attempts + 1;
	if (rc == 0) {
		level = "info";
		body = result.body ? result.body : "Task completed.";
		if (strcmp(task->trigger_type, "interval") == 0 && task->interval_seconds > 0) {
			status = "waiting";
			reason = "interval";
			next_run_at = now + task->interval_seconds;
		} else {
			status = "completed";
			reason = "completed";
		}
	} else if (task->timeout_at > 0 && now >= task->timeout_at) {
		level = "warning";
		status = "timed_out";
		body = result.error ? result.error : "Task timed out.";
		last_error = body;
		reason = "timeout";
		error_code = NB_ERR_TIMEOUT;
	} else if (task->max_attempts > 0 && attempts >= task->max_attempts) {
		level = "warning";
		status = "failed";
		body = result.error ? result.error
				    : nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc);
		last_error = body;
		reason = "max_attempts";
		error_code = rc;
	} else if (task->interval_seconds > 0) {
		level = "warning";
		status = "waiting";
		body = result.error ? result.error
				    : nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc);
		last_error = body;
		reason = "retry";
		error_code = rc;
		next_run_at = now + task_retry_after(task, &result);
	} else {
		level = "warning";
		status = "failed";
		body = result.error ? result.error
				    : nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc);
		last_error = body;
		reason = "runner_error";
		error_code = rc;
	}

	nrc = task_commit_result(db, task, result.session_id, level, body, status, next_run_at,
				 attempts, last_error, notification);
	if (nrc == 0) {
		nrc = task_emit_notification_event(events, task, notification);
	}
	if (nrc == 0)
		nrc = task_emit_result_event(events, task, status, next_run_at, attempts,
					     error_code, reason, notification);
	scheduled_task_action_result_cleanup(&result);
	return nrc;
}

static int run_due_action(struct db *db, const struct scheduled_task *task, int64_t now,
			  scheduled_task_runner_fn runner, void *runner_user_data,
			  struct notification *notification,
			  const struct scheduled_task_event_sink *events)
{
	struct scheduled_task_action_result result;
	const char *body;
	const char *level;
	const char *status;
	const char *last_error = NULL;
	const char *reason = NULL;
	int error_code = 0;
	int attempts;
	int64_t next_run_at = 0;
	int rc;
	int nrc;

	if (!db || !task || !notification)
		return NB_ERR_INVAL;
	if (!runner)
		return run_due_unsupported(db, task, notification, events);

	memset(&result, 0, sizeof(result));
	rc = runner(task, &result, runner_user_data);
	attempts = task->attempts + 1;
	if (rc == 0) {
		level = "info";
		status = "completed";
		body = result.body ? result.body : "Task completed.";
		reason = "completed";
	} else if (task->timeout_at > 0 && now >= task->timeout_at) {
		level = "warning";
		status = "timed_out";
		body = result.error ? result.error : "Task timed out.";
		last_error = body;
		reason = "timeout";
		error_code = NB_ERR_TIMEOUT;
	} else if (action_should_retry(task, attempts)) {
		level = "warning";
		status = "waiting";
		body = result.error ? result.error
				    : nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc);
		last_error = body;
		reason = "retry";
		error_code = rc;
		next_run_at = now + task_retry_after(task, &result);
	} else {
		level = "warning";
		status = "failed";
		body = result.error ? result.error
				    : nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc);
		last_error = body;
		reason = task->max_attempts > 0 && attempts >= task->max_attempts ? "max_attempts"
										  : "runner_error";
		error_code = rc;
	}

	nrc = task_commit_result(db, task, result.session_id, level, body, status, next_run_at,
				 attempts, last_error, notification);
	if (nrc == 0) {
		nrc = task_emit_notification_event(events, task, notification);
	}
	if (nrc == 0)
		nrc = task_emit_result_event(events, task, status, next_run_at, attempts,
					     error_code, reason, notification);
	scheduled_task_action_result_cleanup(&result);
	return nrc;
}

static int run_due_watch(struct db *db, const struct scheduled_task *task, int64_t now,
			 scheduled_task_runner_fn runner, void *runner_user_data,
			 struct notification *notification,
			 const struct scheduled_task_event_sink *events)
{
	struct scheduled_task_action_result result;
	const char *body;
	const char *level;
	const char *status;
	const char *last_error = NULL;
	const char *reason = NULL;
	int error_code = 0;
	int attempts;
	int64_t next_run_at = 0;
	int rc;
	int nrc;

	if (!db || !task || !notification)
		return NB_ERR_INVAL;
	if (!runner)
		return run_due_unsupported(db, task, notification, events);

	memset(&result, 0, sizeof(result));
	rc = runner(task, &result, runner_user_data);
	attempts = task->attempts + 1;
	if (rc == 0 && result.completed) {
		level = "info";
		status = "completed";
		body = result.body ? result.body : "Watch condition completed.";
		reason = "completed";
	} else if (task->timeout_at > 0 && now >= task->timeout_at) {
		level = "warning";
		status = "timed_out";
		body = result.error ? result.error : "Watch task timed out.";
		last_error = body;
		reason = "timeout";
		error_code = NB_ERR_TIMEOUT;
	} else if (task->max_attempts > 0 && attempts >= task->max_attempts) {
		level = "warning";
		status = rc == 0 ? "timed_out" : "failed";
		body = rc == 0 ? "Watch condition did not complete."
			       : (result.error ? result.error
					       : nb_err_str(morph_error_descriptions(),
							    MORPH_ERROR_COUNT, rc));
		last_error = body;
		reason = "max_attempts";
		error_code = rc == 0 ? NB_ERR_TIMEOUT : rc;
	} else {
		level = "info";
		status = "waiting";
		body = result.body ? result.body : "Watch condition not met.";
		last_error = rc == 0 ? NULL
				     : (result.error ? result.error
						     : nb_err_str(morph_error_descriptions(),
								  MORPH_ERROR_COUNT, rc));
		reason = rc == 0 ? "condition_not_met" : "retry";
		error_code = rc;
		next_run_at = now + task_retry_after(task, &result);
	}

	nrc = task_commit_result(db, task, result.session_id, level, body, status, next_run_at,
				 attempts, last_error, notification);
	if (nrc == 0) {
		nrc = task_emit_notification_event(events, task, notification);
	}
	if (nrc == 0)
		nrc = task_emit_result_event(events, task, status, next_run_at, attempts,
					     error_code, reason, notification);
	scheduled_task_action_result_cleanup(&result);
	return nrc;
}

static int run_due_unsupported(struct db *db, const struct scheduled_task *task,
			       struct notification *notification,
			       const struct scheduled_task_event_sink *events)
{
	char body[SCHEDULED_TASK_TEXT_MAX];
	int attempts;
	int rc;

	if (!db || !task)
		return NB_ERR_INVAL;
	snprintf(body, sizeof(body), "Task kind '%s' is due but no runner is registered.",
		 task->kind);
	attempts = task->attempts + 1;
	rc = task_commit_result(db, task, 0, "warning", body, "failed", 0, attempts,
				"no runner registered for task kind", notification);
	if (rc == 0) {
		rc = task_emit_notification_event(events, task, notification);
	}
	if (rc == 0)
		rc = task_emit_result_event(events, task, "failed", 0, attempts, NB_ERR_UNSUPPORTED,
					    "no_runner", notification);
	return rc;
}

struct task_observer {
	struct db *db;
	const struct scheduled_task_event_sink *sink;
};

/** \brief Observe committed state without changing the execution outcome. */
static int task_observe(const struct morph_event *event, void *opaque)
{
	struct task_observer *observer = opaque;
	int rc = observer->sink != NULL && observer->sink->cb != NULL
			 ? observer->sink->cb(event, observer->sink->user_data)
			 : 0;
	if (rc != 0) {
		nb_log_write(observer->db->logger, NB_LOG_WARN, "task observer failed: %s",
			     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	}
	return 0;
}

int scheduled_task_run_due_collect_with_runner_events(
	struct db *db, int64_t now, int limit, scheduled_task_runner_fn runner,
	void *runner_user_data, struct notification **notifications, int *count,
	const struct scheduled_task_event_sink *events)
{
	struct task_observer observer = {.db = db, .sink = events};
	struct scheduled_task_event_sink observations = {.cb = task_observe,
							 .user_data = &observer};
	events = &observations;
	nb_vec arr NB_CLEANUP(notification_array_cleanup) = {0};
	struct task_batch tasks NB_CLEANUP(task_batch_cleanup) = {0};
	int rc;

	if (!db_is_open(db) || now < 0 || !notifications || !count)
		return NB_ERR_INVAL;
	if (*notifications)
		return NB_ERR_EXISTS;
	*count = 0;
	rc = (nb_vec_init(&arr, sizeof(struct notification)),
	      nb_vec_reserve(&arr, 4) ? 0 : NB_ERR_NOMEM);
	if (rc < 0)
		return rc;

	rc = scheduled_task_list_due(db, now, limit, &tasks.items, &tasks.count);
	if (rc != 0)
		return rc;

	for (int i = 0; i < tasks.count; i++) {
		struct notification *notification;
		int claimed;

		if (tasks.items[i].attempts >= INT_MAX || now > INT64_MAX - INT_MAX) {
			return NB_ERR_RANGE;
		}
		claimed = task_claim_due(db, &tasks.items[i]);
		if (claimed < 0) {
			return claimed;
		}
		if (claimed == 0)
			continue;

		rc = task_emit_event(events, "task.claimed", "begin", "task claimed",
				     &tasks.items[i], "running", tasks.items[i].next_run_at,
				     tasks.items[i].attempts, 0, NULL, NULL);
		if (rc != 0)
			return rc;
		rc = task_emit_event(events, "task.started", "begin", "task started",
				     &tasks.items[i], "running", tasks.items[i].next_run_at,
				     tasks.items[i].attempts, 0, NULL, NULL);
		if (rc != 0)
			return rc;

		if (arr.len >= INT_MAX) {
			return NB_ERR_RANGE;
		}
		notification = nb_vec_push(&arr);
		if (!notification) {
			return NB_ERR_NOMEM;
		}
		memset(notification, 0, sizeof(*notification));
		if (strcmp(tasks.items[i].kind, "agent") == 0)
			rc = run_due_agent(db, &tasks.items[i], now, runner, runner_user_data,
					   notification, events);
		else if (strcmp(tasks.items[i].kind, "reminder") == 0)
			rc = run_due_reminder(db, &tasks.items[i], now, notification, events);
		else if (strcmp(tasks.items[i].kind, "action") == 0)
			rc = run_due_action(db, &tasks.items[i], now, runner, runner_user_data,
					    notification, events);
		else if (strcmp(tasks.items[i].kind, "watch") == 0)
			rc = run_due_watch(db, &tasks.items[i], now, runner, runner_user_data,
					   notification, events);
		else
			rc = run_due_unsupported(db, &tasks.items[i], notification, events);
		if (rc != 0)
			return rc;
	}
	if (arr.len > INT_MAX) {
		return NB_ERR_RANGE;
	}
	*notifications = arr.len ? malloc(arr.len * arr.elem_size) : NULL;
	if (arr.len && !*notifications) {
		return NB_ERR_NOMEM;
	}
	if (arr.len)
		memcpy(*notifications, arr.data, arr.len * arr.elem_size);
	*count = (int)arr.len;
	arr.len = 0;
	nb_vec_cleanup(&arr);
	return 0;
}

int scheduled_task_run_due_collect_with_runner(struct db *db, int64_t now, int limit,
					       scheduled_task_runner_fn runner,
					       void *runner_user_data,
					       struct notification **notifications, int *count)
{
	return scheduled_task_run_due_collect_with_runner_events(
		db, now, limit, runner, runner_user_data, notifications, count, NULL);
}

int scheduled_task_run_due_collect(struct db *db, int64_t now, int limit,
				   struct notification **notifications, int *count)
{
	return scheduled_task_run_due_collect_with_runner(db, now, limit, NULL, NULL, notifications,
							  count);
}

int scheduled_task_run_due_with_runner_events(struct db *db, int64_t now, int limit,
					      scheduled_task_runner_fn runner,
					      void *runner_user_data, int *ran,
					      const struct scheduled_task_event_sink *events)
{
	struct notification *notifications = NULL;
	int count = 0;
	int rc;

	rc = scheduled_task_run_due_collect_with_runner_events(
		db, now, limit, runner, runner_user_data, &notifications, &count, events);
	if (rc != 0)
		return rc;
	if (ran)
		*ran = count;
	notification_free_list(notifications, count);
	return 0;
}

int scheduled_task_run_due_with_runner(struct db *db, int64_t now, int limit,
				       scheduled_task_runner_fn runner, void *runner_user_data,
				       int *ran)
{
	return scheduled_task_run_due_with_runner_events(db, now, limit, runner, runner_user_data,
							 ran, NULL);
}

int scheduled_task_run_due(struct db *db, int64_t now, int limit, int *ran)
{
	return scheduled_task_run_due_with_runner(db, now, limit, NULL, NULL, ran);
}

void scheduled_task_action_result_cleanup(struct scheduled_task_action_result *result)
{
	if (!result)
		return;
	free(result->body);
	free(result->error);
	memset(result, 0, sizeof(*result));
}

void scheduled_task_cleanup(struct scheduled_task *task)
{
	if (!task)
		return;
	free(task->payload_json);
	free(task->policy_json);
	free(task->notify_json);
	memset(task, 0, sizeof(*task));
}

void scheduled_task_free_list(struct scheduled_task *tasks, int count)
{
	if (!tasks)
		return;
	for (int i = 0; i < count; i++)
		scheduled_task_cleanup(&tasks[i]);
	free(tasks);
}

int notification_create_for_session(struct db *db, int64_t task_id, int64_t session_id,
				    const char *level, const char *title, const char *body,
				    const char *delivery_status, struct notification *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	struct norm_write_result result = {0};
	int rc;

	if (!db_is_open(db) || !level || !title || !body || !delivery_status)
		return NB_ERR_INVAL;

	if (out && out->body)
		return NB_ERR_EXISTS;

	const struct notification_row row = {
		.task_id = task_id,
		.session_id = session_id,
		.task_missing = task_id <= 0,
		.session_missing = session_id <= 0,
		.level = nb_str_from_cstr(level),
		.title = nb_str_from_cstr(title),
		.body = nb_str_from_cstr(body),
		.created_at = (int64_t)time(NULL),
		.delivery_status = nb_str_from_cstr(delivery_status),
	};

	rc = orm_scope_begin(db, &scope);
	if (rc == NB_OK)
		rc = orm_status(db,
				norm_insert(scope.connection, &notification_model, &row, NULL,
					    orm_scope_call(&scope), &result, &error),
				&error);

	return rc == NB_OK && out ? notification_get(db, result.insert_id, out) : rc;
}

int notification_create(struct db *db, int64_t task_id, const char *level, const char *title,
			const char *body, const char *delivery_status, struct notification *out)
{
	return notification_create_for_session(db, task_id, 0, level, title, body, delivery_status,
					       out);
}

int notification_list_unread(struct db *db, int limit, struct notification **out, int *count)
{
	nb_vec array NB_CLEANUP(notification_array_cleanup) = {0};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	struct norm_order orders[] = {
		{.column = norm_col(&notification_model, 6), .direction = NORM_DESC},
		{.column = norm_col(&notification_model, 0), .direction = NORM_DESC},
	};
	int rc;

	if (!db_is_open(db) || !out || !count)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	*count = 0;
	if (limit <= 0)
		limit = 100;

	struct norm_select_query query = {
		.where = norm_eq(norm_col(&notification_model, 7), norm_i64(0)),
		.order = orders,
		.order_count = sizeof(orders) / sizeof(orders[0]),
		.has_limit = true,
		.limit = (uint64_t)limit,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};

	nb_vec_init(&array, sizeof(struct notification));
	rc = orm_scope_begin(db, &scope);
	if (rc == NB_OK)
		rc = orm_status(db,
				norm_select(scope.connection, &notification_model, &query,
					    orm_scope_call(&scope), &rows, &error),
				&error);

	for (size_t index = 0; rc == NB_OK && index < norm_rows_count(rows); index++) {
		struct notification *notification = nb_vec_push(&array);

		if (!notification)
			return NB_ERR_NOMEM;

		memset(notification, 0, sizeof(*notification));
		rc = notification_from_row(norm_rows_at(rows, index), notification);
	}

	if (rc != NB_OK)
		return rc;

	if (array.len > INT_MAX)
		return NB_ERR_RANGE;

	*out = array.len ? malloc(array.len * array.elem_size) : NULL;
	if (array.len && !*out)
		return NB_ERR_NOMEM;

	if (array.len)
		memcpy(*out, array.data, array.len * array.elem_size);

	*count = (int)array.len;
	array.len = 0;
	return NB_OK;
}

int notification_mark_read(struct db *db, int64_t id, int64_t read_at)
{
	if (!db_is_open(db) || id <= 0 || read_at < 0)
		return NB_ERR_INVAL;

	if (read_at == 0)
		read_at = (int64_t)time(NULL);

	const struct norm_assignment change = {.column = norm_col(&notification_model, 7),
					       .value = norm_i64(read_at)};
	const struct norm_update_query query = {
		.set = &change,
		.set_count = 1,
		.where = norm_eq(norm_col(&notification_model, 0), norm_i64(id))};
	int rc = task_change_count(db, &notification_model, &query);

	return rc < 0 ? rc : (rc ? NB_OK : NB_ERR_NOT_FOUND);
}

void notification_cleanup(struct notification *notification)
{
	if (!notification)
		return;
	free(notification->body);
	memset(notification, 0, sizeof(*notification));
}

void notification_free_list(struct notification *notifications, int count)
{
	if (!notifications)
		return;
	for (int i = 0; i < count; i++)
		notification_cleanup(&notifications[i]);
	free(notifications);
}
