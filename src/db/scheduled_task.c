#include "scheduled_task.h"
#include "util/array.h"
#include "util/error.h"
#include "util/log.h"
#include "cJSON.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int task_emit_event(const struct scheduled_task_event_sink *events,
			   const char *name, const char *phase,
			   const char *message,
			   const struct scheduled_task *task,
			   const char *status, int64_t next_run_at,
			   int attempts, int error_code, const char *reason,
			   const struct notification *notification)
{
	cJSON *data;
	int rc;

	if (!events || !events->cb)
		return 0;
	if (!name || !task)
		MORPH_RETURN(-EINVAL);
	data = cJSON_CreateObject();
	if (!data)
		MORPH_RETURN(-ENOMEM);
	if (!cJSON_AddNumberToObject(data, "task_id", (double)task->id) ||
	    !cJSON_AddNumberToObject(data, "source_session_id",
				     (double)task->source_session_id) ||
	    !cJSON_AddNumberToObject(data, "latest_session_id",
				     (double)task->latest_session_id) ||
	    !cJSON_AddStringToObject(data, "title", task->title) ||
	    !cJSON_AddStringToObject(data, "kind", task->kind) ||
	    !cJSON_AddStringToObject(data, "trigger_type",
				     task->trigger_type) ||
	    !cJSON_AddStringToObject(data, "status",
				     status ? status : task->status) ||
	    !cJSON_AddNumberToObject(data, "next_run_at",
				     (double)next_run_at) ||
	    !cJSON_AddNumberToObject(data, "attempts", attempts) ||
	    !cJSON_AddNumberToObject(data, "max_attempts",
				     task->max_attempts)) {
		cJSON_Delete(data);
		MORPH_RETURN(-ENOMEM);
	}
	if (task->interval_seconds > 0 &&
	    !cJSON_AddNumberToObject(data, "interval_seconds",
				     task->interval_seconds)) {
		cJSON_Delete(data);
		MORPH_RETURN(-ENOMEM);
	}
	if (error_code != 0 &&
	    !cJSON_AddNumberToObject(data, "error_code", error_code)) {
		cJSON_Delete(data);
		MORPH_RETURN(-ENOMEM);
	}
	if (reason &&
	    !cJSON_AddStringToObject(data, "reason", reason)) {
		cJSON_Delete(data);
		MORPH_RETURN(-ENOMEM);
	}
	if (notification) {
		if (!cJSON_AddNumberToObject(data, "notification_id",
					     (double)notification->id) ||
		    !cJSON_AddNumberToObject(data, "session_id",
					     (double)notification->session_id) ||
		    !cJSON_AddStringToObject(data, "notification_level",
					     notification->level) ||
		    !cJSON_AddStringToObject(data, "notification_title",
					     notification->title) ||
		    !cJSON_AddStringToObject(data, "notification_body",
					     notification->body ?
					     notification->body : "")) {
			cJSON_Delete(data);
			MORPH_RETURN(-ENOMEM);
		}
	}
	rc = morph_event_emit_simple(events->cb, events->user_data,
				     MORPH_EVENT_TASK, name, phase, message,
				     data);
	cJSON_Delete(data);
	return rc;
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
	if (!dst || dst_size == 0)
		return;
	dst[0] = '\0';
	if (!src)
		return;
	strncpy(dst, src, dst_size - 1);
	dst[dst_size - 1] = '\0';
}

static char *column_strdup(sqlite3_stmt *stmt, int col)
{
	const unsigned char *text = sqlite3_column_text(stmt, col);

	if (!text)
		return NULL;
	return strdup((const char *)text);
}

static int task_from_stmt(sqlite3_stmt *stmt, struct scheduled_task *task)
{
	if (!stmt || !task)
		MORPH_RETURN(-EINVAL);

	memset(task, 0, sizeof(*task));
	task->id = sqlite3_column_int64(stmt, 0);
	task->source_session_id = sqlite3_column_int64(stmt, 1);
	task->latest_session_id = sqlite3_column_int64(stmt, 2);
	copy_text(task->title, sizeof(task->title),
		  (const char *)sqlite3_column_text(stmt, 3));
	copy_text(task->kind, sizeof(task->kind),
		  (const char *)sqlite3_column_text(stmt, 4));
	copy_text(task->status, sizeof(task->status),
		  (const char *)sqlite3_column_text(stmt, 5));
	copy_text(task->trigger_type, sizeof(task->trigger_type),
		  (const char *)sqlite3_column_text(stmt, 6));
	task->next_run_at = sqlite3_column_int64(stmt, 7);
	task->interval_seconds = sqlite3_column_int(stmt, 8);
	task->timeout_at = sqlite3_column_int64(stmt, 9);
	task->attempts = sqlite3_column_int(stmt, 10);
	task->max_attempts = sqlite3_column_int(stmt, 11);
	copy_text(task->action_type, sizeof(task->action_type),
		  (const char *)sqlite3_column_text(stmt, 12));
	task->payload_json = column_strdup(stmt, 13);
	task->policy_json = column_strdup(stmt, 14);
	task->notify_json = column_strdup(stmt, 15);
	copy_text(task->last_error, sizeof(task->last_error),
		  (const char *)sqlite3_column_text(stmt, 16));
	task->created_at = sqlite3_column_int64(stmt, 17);
	task->updated_at = sqlite3_column_int64(stmt, 18);

	if ((sqlite3_column_type(stmt, 13) != SQLITE_NULL && !task->payload_json) ||
	    (sqlite3_column_type(stmt, 14) != SQLITE_NULL && !task->policy_json) ||
	    (sqlite3_column_type(stmt, 15) != SQLITE_NULL && !task->notify_json)) {
		scheduled_task_cleanup(task);
		MORPH_RETURN(-ENOMEM);
	}
	return 0;
}

static int notification_from_stmt(sqlite3_stmt *stmt,
				  struct notification *notification)
{
	if (!stmt || !notification)
		MORPH_RETURN(-EINVAL);

	memset(notification, 0, sizeof(*notification));
	notification->id = sqlite3_column_int64(stmt, 0);
	notification->task_id = sqlite3_column_int64(stmt, 1);
	notification->session_id = sqlite3_column_int64(stmt, 2);
	copy_text(notification->level, sizeof(notification->level),
		  (const char *)sqlite3_column_text(stmt, 3));
	copy_text(notification->title, sizeof(notification->title),
		  (const char *)sqlite3_column_text(stmt, 4));
	notification->body = column_strdup(stmt, 5);
	notification->created_at = sqlite3_column_int64(stmt, 6);
	notification->read_at = sqlite3_column_int64(stmt, 7);
	copy_text(notification->delivery_status,
		  sizeof(notification->delivery_status),
		  (const char *)sqlite3_column_text(stmt, 8));

	if (!notification->body)
		MORPH_RETURN(-ENOMEM);
	return 0;
}

static int notification_get(struct db *db, int64_t id,
			    struct notification *notification)
{
	sqlite3_stmt *stmt = NULL;
	int rc;
	const char *sql =
		"SELECT id,task_id,session_id,level,title,body,created_at,read_at,"
		"delivery_status FROM notifications WHERE id=?";

	if (!db || !db->handle || id <= 0 || !notification)
		MORPH_RETURN(-EINVAL);
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, id);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		rc = notification_from_stmt(stmt, notification);
		sqlite3_finalize(stmt);
		return rc;
	}
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	MORPH_RETURN(-ENOENT);
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

static int scheduled_task_validate_input(
	const struct scheduled_task_input *input)
{
	static const char *const kinds[] = {
		"agent", "reminder", "action", "watch"
	};
	static const char *const triggers[] = {
		"once", "interval", "cron", "event"
	};

	if (!input || !input->title || !input->kind ||
	    !input->trigger_type || !input->action_type)
		MORPH_RETURN(-EINVAL);
	if (!input->title[0] || !input->kind[0] ||
	    !input->trigger_type[0] || !input->action_type[0])
		MORPH_RETURN(-EINVAL);
	if (strlen(input->title) >= SCHEDULED_TASK_TEXT_MAX ||
	    strlen(input->kind) >= SCHEDULED_TASK_TYPE_MAX ||
	    strlen(input->trigger_type) >= SCHEDULED_TASK_TYPE_MAX ||
	    strlen(input->action_type) >= SCHEDULED_TASK_TYPE_MAX)
		MORPH_RETURN(-ENAMETOOLONG);
	if (!str_in_set(input->kind, kinds, (int)(sizeof(kinds) /
	    sizeof(kinds[0]))))
		MORPH_RETURN(-EINVAL);
	if (!str_in_set(input->trigger_type, triggers,
	    (int)(sizeof(triggers) / sizeof(triggers[0]))))
		MORPH_RETURN(-EINVAL);
	if (input->next_run_at < 0 || input->interval_seconds < 0 ||
	    input->timeout_at < 0 || input->max_attempts < 0)
		MORPH_RETURN(-EINVAL);
	if (strcmp(input->trigger_type, "interval") == 0 &&
	    input->interval_seconds <= 0)
		MORPH_RETURN(-EINVAL);
	if ((strcmp(input->kind, "agent") == 0 ||
	     strcmp(input->kind, "action") == 0 ||
	     strcmp(input->kind, "watch") == 0) && !input->payload_json)
		MORPH_RETURN(-EINVAL);
	return 0;
}

static int task_list_query(struct db *db, const char *sql, const char *status,
			   int64_t now, int limit, struct scheduled_task **out,
			   int *count)
{
	sqlite3_stmt *stmt = NULL;
	morph_array_t arr;
	int arr_ready = 0;
	int bind_idx = 1;
	int rc;

	if (!db || !db->handle || !sql || !out || !count)
		MORPH_RETURN(-EINVAL);
	if (limit <= 0)
		limit = 100;

	*out = NULL;
	*count = 0;
	rc = morph_array_init(&arr, 8, sizeof(struct scheduled_task));
	if (rc < 0)
		return rc;
	arr_ready = 1;

	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		rc = MORPH_ERR_DB;
		goto out_fail;
	}
	if (status)
		sqlite3_bind_text(stmt, bind_idx++, status, -1, SQLITE_TRANSIENT);
	if (now >= 0)
		sqlite3_bind_int64(stmt, bind_idx++, now);
	sqlite3_bind_int(stmt, bind_idx, limit);

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		struct scheduled_task *task = morph_array_push(&arr);
		if (!task) {
			rc = -ENOMEM;
			goto out_fail;
		}
		rc = task_from_stmt(stmt, task);
		if (rc != 0)
			goto out_fail;
	}
	if (rc != SQLITE_DONE) {
		rc = MORPH_ERR_DB;
		goto out_fail;
	}
	sqlite3_finalize(stmt);
	if (arr.nelts > INT_MAX) {
		scheduled_task_free_list(arr.elts, (int)arr.nelts);
		MORPH_RETURN(-EOVERFLOW);
	}
	*out = arr.elts;
	*count = (int)arr.nelts;
	return 0;

out_fail:
	if (stmt)
		sqlite3_finalize(stmt);
	if (arr_ready)
		scheduled_task_free_list(arr.elts, (int)arr.nelts);
	MORPH_RETURN(rc);
}

int scheduled_task_create_with_events(
	struct db *db, const struct scheduled_task_input *input,
	struct scheduled_task *out, const struct scheduled_task_event_sink *events)
{
	sqlite3_stmt *stmt = NULL;
	struct scheduled_task task;
	int task_ready = 0;
	int64_t now;
	int rc;
	const char *sql =
		"INSERT INTO scheduled_tasks("
		"source_session_id,latest_session_id,title,kind,status,"
		"trigger_type,next_run_at,interval_seconds,"
		"timeout_at,attempts,max_attempts,action_type,payload_json,"
		"policy_json,notify_json,last_error,created_at,updated_at)"
		" VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

	if (!db || !db->handle)
		MORPH_RETURN(-EINVAL);
	rc = scheduled_task_validate_input(input);
	if (rc != 0)
		return rc;

	now = (int64_t)time(NULL);
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);

	if (input->source_session_id > 0)
		sqlite3_bind_int64(stmt, 1, input->source_session_id);
	else
		sqlite3_bind_null(stmt, 1);
	sqlite3_bind_null(stmt, 2);
	sqlite3_bind_text(stmt, 3, input->title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, input->kind, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, "pending", -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 6, input->trigger_type, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 7, input->next_run_at);
	sqlite3_bind_int(stmt, 8, input->interval_seconds);
	sqlite3_bind_int64(stmt, 9, input->timeout_at);
	sqlite3_bind_int(stmt, 10, 0);
	sqlite3_bind_int(stmt, 11, input->max_attempts);
	sqlite3_bind_text(stmt, 12, input->action_type, -1, SQLITE_TRANSIENT);
	if (input->payload_json)
		sqlite3_bind_text(stmt, 13, input->payload_json, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 13);
	if (input->policy_json)
		sqlite3_bind_text(stmt, 14, input->policy_json, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 14);
	if (input->notify_json)
		sqlite3_bind_text(stmt, 15, input->notify_json, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 15);
	sqlite3_bind_null(stmt, 16);
	sqlite3_bind_int64(stmt, 17, now);
	sqlite3_bind_int64(stmt, 18, now);

	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE) {
		log_err("scheduled task create failed: %s",
			sqlite3_errmsg(db->handle));
		MORPH_RETURN(MORPH_ERR_DB);
	}

	memset(&task, 0, sizeof(task));
	rc = scheduled_task_get(db, sqlite3_last_insert_rowid(db->handle),
				&task);
	if (rc != 0)
		return rc;
	task_ready = 1;
	rc = task_emit_event(events, "task.created", "end", "task created",
			     &task, task.status, task.next_run_at,
			     task.attempts, 0, NULL, NULL);
	if (rc == 0 && out) {
		*out = task;
		task_ready = 0;
	}
	if (task_ready)
		scheduled_task_cleanup(&task);
	return rc;
}

int scheduled_task_create(struct db *db,
			  const struct scheduled_task_input *input,
			  struct scheduled_task *out)
{
	return scheduled_task_create_with_events(db, input, out, NULL);
}

int scheduled_task_get(struct db *db, int64_t id, struct scheduled_task *out)
{
	sqlite3_stmt *stmt = NULL;
	int rc;
	const char *sql =
		"SELECT id,source_session_id,latest_session_id,title,kind,"
		"status,trigger_type,next_run_at,"
		"interval_seconds,timeout_at,attempts,max_attempts,action_type,"
		"payload_json,policy_json,notify_json,last_error,created_at,"
		"updated_at FROM scheduled_tasks WHERE id=?";

	if (!db || !db->handle || id <= 0 || !out)
		MORPH_RETURN(-EINVAL);
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, id);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		rc = task_from_stmt(stmt, out);
		sqlite3_finalize(stmt);
		return rc;
	}
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	MORPH_RETURN(-ENOENT);
}

int scheduled_task_update_with_events(
	struct db *db, int64_t id, const struct scheduled_task_input *input,
	struct scheduled_task *out, const struct scheduled_task_event_sink *events)
{
	sqlite3_stmt *stmt = NULL;
	struct scheduled_task task;
	int task_ready = 0;
	int64_t now;
	int rc;
	const char *sql =
		"UPDATE scheduled_tasks SET title=?,kind=?,status='pending',"
		"trigger_type=?,"
		"next_run_at=?,interval_seconds=?,timeout_at=?,max_attempts=?,"
		"action_type=?,payload_json=?,policy_json=?,notify_json=?,"
		"attempts=0,last_error=NULL,updated_at=? WHERE id=?";

	if (!db || !db->handle || id <= 0)
		MORPH_RETURN(-EINVAL);
	rc = scheduled_task_validate_input(input);
	if (rc != 0)
		return rc;
	now = (int64_t)time(NULL);
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_text(stmt, 1, input->title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, input->kind, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, input->trigger_type, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 4, input->next_run_at);
	sqlite3_bind_int(stmt, 5, input->interval_seconds);
	sqlite3_bind_int64(stmt, 6, input->timeout_at);
	sqlite3_bind_int(stmt, 7, input->max_attempts);
	sqlite3_bind_text(stmt, 8, input->action_type, -1, SQLITE_TRANSIENT);
	if (input->payload_json)
		sqlite3_bind_text(stmt, 9, input->payload_json, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 9);
	if (input->policy_json)
		sqlite3_bind_text(stmt, 10, input->policy_json, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 10);
	if (input->notify_json)
		sqlite3_bind_text(stmt, 11, input->notify_json, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 11);
	sqlite3_bind_int64(stmt, 12, now);
	sqlite3_bind_int64(stmt, 13, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	if (sqlite3_changes(db->handle) == 0)
		MORPH_RETURN(-ENOENT);
	memset(&task, 0, sizeof(task));
	rc = scheduled_task_get(db, id, &task);
	if (rc != 0)
		return rc;
	task_ready = 1;
	rc = task_emit_event(events, "task.updated", "end", "task updated",
			     &task, task.status, task.next_run_at,
			     task.attempts, 0, NULL, NULL);
	if (rc == 0 && out) {
		*out = task;
		task_ready = 0;
	}
	if (task_ready)
		scheduled_task_cleanup(&task);
	return rc;
}

int scheduled_task_update(struct db *db, int64_t id,
			  const struct scheduled_task_input *input,
			  struct scheduled_task *out)
{
	return scheduled_task_update_with_events(db, id, input, out, NULL);
}

int scheduled_task_list(struct db *db, const char *status, int limit,
			struct scheduled_task **out, int *count)
{
	const char *sql_all =
		"SELECT id,source_session_id,latest_session_id,title,kind,"
		"status,trigger_type,next_run_at,"
		"interval_seconds,timeout_at,attempts,max_attempts,action_type,"
		"payload_json,policy_json,notify_json,last_error,created_at,"
		"updated_at FROM scheduled_tasks "
		"ORDER BY updated_at DESC, id DESC LIMIT ?";
	const char *sql_status =
		"SELECT id,source_session_id,latest_session_id,title,kind,"
		"status,trigger_type,next_run_at,"
		"interval_seconds,timeout_at,attempts,max_attempts,action_type,"
		"payload_json,policy_json,notify_json,last_error,created_at,"
		"updated_at FROM scheduled_tasks WHERE status=? "
		"ORDER BY updated_at DESC, id DESC LIMIT ?";

	return task_list_query(db, status ? sql_status : sql_all, status, -1,
			       limit, out, count);
}

int scheduled_task_list_due(struct db *db, int64_t now, int limit,
			    struct scheduled_task **out, int *count)
{
	const char *sql =
		"SELECT id,source_session_id,latest_session_id,title,kind,"
		"status,trigger_type,next_run_at,"
		"interval_seconds,timeout_at,attempts,max_attempts,action_type,"
		"payload_json,policy_json,notify_json,last_error,created_at,"
		"updated_at FROM scheduled_tasks "
		"WHERE status IN ('pending','waiting') AND next_run_at <= ? "
		"ORDER BY next_run_at ASC, id ASC LIMIT ?";

	if (!db || !db->handle || now < 0 || !out || !count)
		MORPH_RETURN(-EINVAL);
	return task_list_query(db, sql, NULL, now, limit, out, count);
}

int scheduled_task_update_run(struct db *db, int64_t id, const char *status,
			      int64_t next_run_at, int attempts,
			      int64_t latest_session_id,
			      const char *last_error)
{
	sqlite3_stmt *stmt = NULL;
	int64_t now;
	int rc;
	const char *sql =
		"UPDATE scheduled_tasks SET status=?,next_run_at=?,attempts=?,"
		"latest_session_id=CASE WHEN ? > 0 THEN ? ELSE latest_session_id END,"
		"last_error=?,updated_at=? WHERE id=?";

	if (!db || !db->handle || id <= 0 || !status || next_run_at < 0 ||
	    attempts < 0)
		MORPH_RETURN(-EINVAL);
	now = (int64_t)time(NULL);
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, next_run_at);
	sqlite3_bind_int(stmt, 3, attempts);
	sqlite3_bind_int64(stmt, 4, latest_session_id);
	sqlite3_bind_int64(stmt, 5, latest_session_id);
	if (last_error)
		sqlite3_bind_text(stmt, 6, last_error, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 6);
	sqlite3_bind_int64(stmt, 7, now);
	sqlite3_bind_int64(stmt, 8, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	if (sqlite3_changes(db->handle) == 0)
		MORPH_RETURN(-ENOENT);
	return 0;
}

int scheduled_task_recover_stale_running(struct db *db, int64_t now,
					 int stale_after_seconds,
					 int *recovered)
{
	sqlite3_stmt *stmt = NULL;
	int rc;
	int64_t cutoff;
	const char *sql =
		"UPDATE scheduled_tasks SET status='waiting',next_run_at=?,"
		"last_error='Recovered after interrupted background execution',"
		"updated_at=? WHERE status='running' AND updated_at <= ?";

	if (!db || !db->handle || now < 0 || stale_after_seconds < 0)
		MORPH_RETURN(-EINVAL);
	cutoff = now - stale_after_seconds;
	if (cutoff < 0)
		cutoff = 0;
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, now);
	sqlite3_bind_int64(stmt, 2, now);
	sqlite3_bind_int64(stmt, 3, cutoff);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	if (recovered)
		*recovered = sqlite3_changes(db->handle);
	return 0;
}

int scheduled_task_cancel_with_events(
	struct db *db, int64_t id, const struct scheduled_task_event_sink *events)
{
	sqlite3_stmt *stmt = NULL;
	struct scheduled_task task;
	int64_t now;
	int rc;
	const char *sql =
		"UPDATE scheduled_tasks SET status='cancelled',updated_at=? "
		"WHERE id=?";

	if (!db || !db->handle || id <= 0)
		MORPH_RETURN(-EINVAL);
	now = (int64_t)time(NULL);
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, now);
	sqlite3_bind_int64(stmt, 2, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	if (sqlite3_changes(db->handle) == 0)
		MORPH_RETURN(-ENOENT);
	memset(&task, 0, sizeof(task));
	rc = scheduled_task_get(db, id, &task);
	if (rc != 0)
		return rc;
	rc = task_emit_event(events, "task.cancelled", "cancelled",
			     "task cancelled", &task, task.status,
			     task.next_run_at, task.attempts, -ECANCELED,
			     "cancelled", NULL);
	scheduled_task_cleanup(&task);
	return rc;
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

static int task_claim_due(struct db *db, const struct scheduled_task *task)
{
	sqlite3_stmt *stmt = NULL;
	int64_t now;
	int rc;
	const char *sql =
		"UPDATE scheduled_tasks SET status='running',updated_at=? "
		"WHERE id=? AND status IN ('pending','waiting')";

	if (!db || !db->handle || !task)
		MORPH_RETURN(-EINVAL);
	now = (int64_t)time(NULL);
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, now);
	sqlite3_bind_int64(stmt, 2, task->id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	return sqlite3_changes(db->handle) > 0 ? 1 : 0;
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
		cJSON_ArrayForEach(target, targets) {
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

static int task_finish_or_reschedule(struct db *db,
				     const struct scheduled_task *task,
				     int64_t now)
{
	int attempts;
	int64_t next_run_at;

	if (!db || !task)
		MORPH_RETURN(-EINVAL);
	attempts = task->attempts + 1;
	if (task->timeout_at > 0 && now >= task->timeout_at)
		return scheduled_task_update_run(db, task->id, "timed_out", 0,
						 attempts, 0, "task timed out");
	if (task->max_attempts > 0 && attempts >= task->max_attempts)
		return scheduled_task_update_run(db, task->id, "completed", 0,
						 attempts, 0, NULL);
	if (task->interval_seconds > 0) {
		next_run_at = now + task->interval_seconds;
		return scheduled_task_update_run(db, task->id, "waiting",
						 next_run_at, attempts, 0, NULL);
	}
	return scheduled_task_update_run(db, task->id, "completed", 0,
					 attempts, 0, NULL);
}

static int task_emit_notification_event(
	const struct scheduled_task_event_sink *events,
	const struct scheduled_task *task, const struct notification *notification);
static int task_emit_result_event(
	const struct scheduled_task_event_sink *events,
	const struct scheduled_task *task, const char *status,
	int64_t next_run_at, int attempts, int error_code,
	const char *reason, const struct notification *notification);

static int run_due_reminder(struct db *db, const struct scheduled_task *task,
			    int64_t now, struct notification *notification,
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
		MORPH_RETURN(-ENOMEM);

	rc = notification_create(db, task->id, "info", task->title, body,
				 task_delivery_status(task), notification);
	free(body);
	if (rc != 0)
		return rc;
	rc = task_emit_notification_event(events, task, notification);
	if (rc != 0)
		return rc;
	attempts = task->attempts + 1;
	if (task->timeout_at > 0 && now >= task->timeout_at) {
		status = "timed_out";
		reason = "timeout";
	} else if (task->interval_seconds > 0) {
		status = "waiting";
		reason = "interval";
		next_run_at = now + task->interval_seconds;
	} else {
		status = "completed";
		reason = "completed";
	}
	rc = task_finish_or_reschedule(db, task, now);
	if (rc == 0)
		rc = task_emit_result_event(events, task, status, next_run_at,
					    attempts,
					    strcmp(status, "timed_out") == 0 ?
					    -ETIMEDOUT : 0,
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

static int task_emit_notification_event(
	const struct scheduled_task_event_sink *events,
	const struct scheduled_task *task, const struct notification *notification)
{
	if (!notification)
		return 0;
	return task_emit_event(events, "task.notification", "end",
			       "task notification created", task, task->status,
			       task->next_run_at, task->attempts, 0, NULL,
			       notification);
}

static int task_emit_result_event(
	const struct scheduled_task_event_sink *events,
	const struct scheduled_task *task, const char *status,
	int64_t next_run_at, int attempts, int error_code,
	const char *reason, const struct notification *notification)
{
	const char *name = "task.updated";
	const char *phase = "end";
	const char *message = "task updated";

	if (!status)
		MORPH_RETURN(-EINVAL);
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
	} else if (strcmp(status, "failed") == 0 &&
		   reason && strcmp(reason, "max_attempts") == 0) {
		name = "task.max_attempts_reached";
		phase = "failed";
		message = "task reached maximum attempts";
	} else if (strcmp(status, "failed") == 0) {
		name = "task.failed";
		phase = "failed";
		message = "task failed";
	}
	return task_emit_event(events, name, phase, message, task, status,
			       next_run_at, attempts, error_code, reason,
			       notification);
}

static int run_due_agent(struct db *db, const struct scheduled_task *task,
			 int64_t now, scheduled_task_runner_fn runner,
			 void *runner_user_data,
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
		MORPH_RETURN(-EINVAL);
	if (!runner)
		return run_due_unsupported(db, task, notification, events);

	memset(&result, 0, sizeof(result));
	rc = runner(task, &result, runner_user_data);
	attempts = task->attempts + 1;
	if (rc == 0) {
		level = "info";
		body = result.body ? result.body : "Task completed.";
		if (strcmp(task->trigger_type, "interval") == 0 &&
		    task->interval_seconds > 0) {
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
		error_code = -ETIMEDOUT;
	} else if (task->max_attempts > 0 && attempts >= task->max_attempts) {
		level = "warning";
		status = "failed";
		body = result.error ? result.error : morph_strerror(rc);
		last_error = body;
		reason = "max_attempts";
		error_code = rc;
	} else if (task->interval_seconds > 0) {
		level = "warning";
		status = "waiting";
		body = result.error ? result.error : morph_strerror(rc);
		last_error = body;
		reason = "retry";
		error_code = rc;
		next_run_at = now + task_retry_after(task, &result);
	} else {
		level = "warning";
		status = "failed";
		body = result.error ? result.error : morph_strerror(rc);
		last_error = body;
		reason = "runner_error";
		error_code = rc;
	}

	nrc = notification_create_for_session(db, task->id, result.session_id,
					      level, task->title, body,
					      task_delivery_status(task),
					      notification);
	if (nrc == 0)
		nrc = task_emit_notification_event(events, task, notification);
	if (nrc == 0)
		nrc = scheduled_task_update_run(db, task->id, status,
						next_run_at, attempts,
						result.session_id,
						last_error);
	if (nrc == 0)
		nrc = task_emit_result_event(events, task, status, next_run_at,
					     attempts, error_code, reason,
					     notification);
	scheduled_task_action_result_cleanup(&result);
	return nrc;
}

static int run_due_action(struct db *db, const struct scheduled_task *task,
			  int64_t now, scheduled_task_runner_fn runner,
			  void *runner_user_data,
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
		MORPH_RETURN(-EINVAL);
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
		error_code = -ETIMEDOUT;
	} else if (action_should_retry(task, attempts)) {
		level = "warning";
		status = "waiting";
		body = result.error ? result.error : morph_strerror(rc);
		last_error = body;
		reason = "retry";
		error_code = rc;
		next_run_at = now + task_retry_after(task, &result);
	} else {
		level = "warning";
		status = "failed";
		body = result.error ? result.error : morph_strerror(rc);
		last_error = body;
		reason = task->max_attempts > 0 && attempts >= task->max_attempts ?
			"max_attempts" : "runner_error";
		error_code = rc;
	}

	nrc = notification_create_for_session(db, task->id, result.session_id,
					      level, task->title, body,
					      task_delivery_status(task),
					      notification);
	if (nrc == 0)
		nrc = task_emit_notification_event(events, task, notification);
	if (nrc == 0)
		nrc = scheduled_task_update_run(db, task->id, status,
						next_run_at, attempts,
						result.session_id,
						last_error);
	if (nrc == 0)
		nrc = task_emit_result_event(events, task, status, next_run_at,
					     attempts, error_code, reason,
					     notification);
	scheduled_task_action_result_cleanup(&result);
	return nrc;
}

static int run_due_watch(struct db *db, const struct scheduled_task *task,
			 int64_t now, scheduled_task_runner_fn runner,
			 void *runner_user_data,
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
		MORPH_RETURN(-EINVAL);
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
		error_code = -ETIMEDOUT;
	} else if (task->max_attempts > 0 && attempts >= task->max_attempts) {
		level = "warning";
		status = rc == 0 ? "timed_out" : "failed";
		body = rc == 0 ? "Watch condition did not complete." :
			(result.error ? result.error : morph_strerror(rc));
		last_error = body;
		reason = "max_attempts";
		error_code = rc == 0 ? -ETIMEDOUT : rc;
	} else {
		level = "info";
		status = "waiting";
		body = result.body ? result.body : "Watch condition not met.";
		last_error = rc == 0 ? NULL :
			(result.error ? result.error : morph_strerror(rc));
		reason = rc == 0 ? "condition_not_met" : "retry";
		error_code = rc;
		next_run_at = now + task_retry_after(task, &result);
	}

	nrc = notification_create_for_session(db, task->id, result.session_id,
					      level, task->title, body,
					      task_delivery_status(task),
					      notification);
	if (nrc == 0)
		nrc = task_emit_notification_event(events, task, notification);
	if (nrc == 0)
		nrc = scheduled_task_update_run(db, task->id, status,
						next_run_at, attempts,
						result.session_id,
						last_error);
	if (nrc == 0)
		nrc = task_emit_result_event(events, task, status, next_run_at,
					     attempts, error_code, reason,
					     notification);
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
		MORPH_RETURN(-EINVAL);
	snprintf(body, sizeof(body),
		 "Task kind '%s' is due but no runner is registered.",
		 task->kind);
	rc = notification_create(db, task->id, "warning", task->title, body,
				 task_delivery_status(task), notification);
	if (rc != 0)
		return rc;
	rc = task_emit_notification_event(events, task, notification);
	if (rc != 0)
		return rc;
	attempts = task->attempts + 1;
	rc = scheduled_task_update_run(db, task->id, "failed", 0, attempts,
				       0, "no runner registered for task kind");
	if (rc == 0)
		rc = task_emit_result_event(events, task, "failed", 0,
					    attempts, -ENOSYS, "no_runner",
					    notification);
	return rc;
}

int scheduled_task_run_due_collect_with_runner_events(
	struct db *db, int64_t now, int limit,
	scheduled_task_runner_fn runner, void *runner_user_data,
	struct notification **notifications, int *count,
	const struct scheduled_task_event_sink *events)
{
	struct scheduled_task *tasks = NULL;
	morph_array_t arr;
	int arr_ready = 0;
	int task_count = 0;
	int rc;

	if (!db || !db->handle || now < 0 || !notifications || !count)
		MORPH_RETURN(-EINVAL);
	*notifications = NULL;
	*count = 0;
	rc = morph_array_init(&arr, 4, sizeof(struct notification));
	if (rc < 0)
		return rc;
	arr_ready = 1;

	rc = scheduled_task_list_due(db, now, limit, &tasks, &task_count);
	if (rc != 0)
		goto out_fail;

	for (int i = 0; i < task_count; i++) {
		struct notification *notification;
		int claimed;

		claimed = task_claim_due(db, &tasks[i]);
		if (claimed < 0) {
			rc = claimed;
			goto out_fail;
		}
		if (claimed == 0)
			continue;

		rc = task_emit_event(events, "task.claimed", "begin",
				     "task claimed", &tasks[i], "running",
				     tasks[i].next_run_at, tasks[i].attempts,
				     0, NULL, NULL);
		if (rc != 0)
			goto out_fail;
		rc = task_emit_event(events, "task.started", "begin",
				     "task started", &tasks[i], "running",
				     tasks[i].next_run_at, tasks[i].attempts,
				     0, NULL, NULL);
		if (rc != 0)
			goto out_fail;

		notification = morph_array_push(&arr);
		if (!notification) {
			rc = -ENOMEM;
			goto out_fail;
		}
		if (strcmp(tasks[i].kind, "agent") == 0)
			rc = run_due_agent(db, &tasks[i], now, runner,
					   runner_user_data, notification,
					   events);
		else if (strcmp(tasks[i].kind, "reminder") == 0)
			rc = run_due_reminder(db, &tasks[i], now,
					      notification, events);
		else if (strcmp(tasks[i].kind, "action") == 0)
			rc = run_due_action(db, &tasks[i], now, runner,
					    runner_user_data, notification,
					    events);
		else if (strcmp(tasks[i].kind, "watch") == 0)
			rc = run_due_watch(db, &tasks[i], now, runner,
					   runner_user_data, notification,
					   events);
		else
			rc = run_due_unsupported(db, &tasks[i],
						 notification, events);
		if (rc != 0)
			goto out_fail;
	}
	scheduled_task_free_list(tasks, task_count);
	if (arr.nelts > INT_MAX) {
		notification_free_list(arr.elts, (int)arr.nelts);
		MORPH_RETURN(-EOVERFLOW);
	}
	*notifications = arr.elts;
	*count = (int)arr.nelts;
	return 0;

out_fail:
	scheduled_task_free_list(tasks, task_count);
	if (arr_ready)
		notification_free_list(arr.elts, (int)arr.nelts);
	MORPH_RETURN(rc);
}

int scheduled_task_run_due_collect_with_runner(
	struct db *db, int64_t now, int limit,
	scheduled_task_runner_fn runner, void *runner_user_data,
	struct notification **notifications, int *count)
{
	return scheduled_task_run_due_collect_with_runner_events(
		db, now, limit, runner, runner_user_data, notifications, count,
		NULL);
}

int scheduled_task_run_due_collect(struct db *db, int64_t now, int limit,
				   struct notification **notifications,
				   int *count)
{
	return scheduled_task_run_due_collect_with_runner(
		db, now, limit, NULL, NULL, notifications, count);
}

int scheduled_task_run_due_with_runner_events(
	struct db *db, int64_t now, int limit, scheduled_task_runner_fn runner,
	void *runner_user_data, int *ran,
	const struct scheduled_task_event_sink *events)
{
	struct notification *notifications = NULL;
	int count = 0;
	int rc;

	rc = scheduled_task_run_due_collect_with_runner_events(
		db, now, limit, runner, runner_user_data, &notifications,
		&count, events);
	if (rc != 0)
		return rc;
	if (ran)
		*ran = count;
	notification_free_list(notifications, count);
	return 0;
}

int scheduled_task_run_due_with_runner(struct db *db, int64_t now, int limit,
				       scheduled_task_runner_fn runner,
				       void *runner_user_data, int *ran)
{
	return scheduled_task_run_due_with_runner_events(
		db, now, limit, runner, runner_user_data, ran, NULL);
}

int scheduled_task_run_due(struct db *db, int64_t now, int limit, int *ran)
{
	return scheduled_task_run_due_with_runner(db, now, limit, NULL, NULL,
						 ran);
}

void scheduled_task_action_result_cleanup(
	struct scheduled_task_action_result *result)
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

int notification_create_for_session(struct db *db, int64_t task_id,
				    int64_t session_id, const char *level,
				    const char *title, const char *body,
				    const char *delivery_status,
				    struct notification *out)
{
	sqlite3_stmt *stmt = NULL;
	int64_t now;
	int rc;
	const char *sql =
		"INSERT INTO notifications(task_id,session_id,level,title,body,"
		"created_at,read_at,delivery_status) VALUES(?,?,?,?,?,?,?,?)";

	if (!db || !db->handle || !level || !title || !body ||
	    !delivery_status)
		MORPH_RETURN(-EINVAL);
	now = (int64_t)time(NULL);
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	if (task_id > 0)
		sqlite3_bind_int64(stmt, 1, task_id);
	else
		sqlite3_bind_null(stmt, 1);
	if (session_id > 0)
		sqlite3_bind_int64(stmt, 2, session_id);
	else
		sqlite3_bind_null(stmt, 2);
	sqlite3_bind_text(stmt, 3, level, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, body, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 6, now);
	sqlite3_bind_int64(stmt, 7, 0);
	sqlite3_bind_text(stmt, 8, delivery_status, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);

	if (out)
		return notification_get(db, sqlite3_last_insert_rowid(db->handle),
					out);
	return 0;
}

int notification_create(struct db *db, int64_t task_id, const char *level,
			const char *title, const char *body,
			const char *delivery_status,
			struct notification *out)
{
	return notification_create_for_session(db, task_id, 0, level, title,
					       body, delivery_status, out);
}

int notification_list_unread(struct db *db, int limit,
			     struct notification **out, int *count)
{
	sqlite3_stmt *stmt = NULL;
	morph_array_t arr;
	int arr_ready = 0;
	int rc;
	const char *sql =
		"SELECT id,task_id,session_id,level,title,body,created_at,read_at,"
		"delivery_status FROM notifications WHERE read_at = 0 "
		"ORDER BY created_at DESC, id DESC LIMIT ?";

	if (!db || !db->handle || !out || !count)
		MORPH_RETURN(-EINVAL);
	if (limit <= 0)
		limit = 100;

	*out = NULL;
	*count = 0;
	rc = morph_array_init(&arr, 8, sizeof(struct notification));
	if (rc < 0)
		return rc;
	arr_ready = 1;

	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		rc = MORPH_ERR_DB;
		goto out_fail;
	}
	sqlite3_bind_int(stmt, 1, limit);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		struct notification *notification = morph_array_push(&arr);
		if (!notification) {
			rc = -ENOMEM;
			goto out_fail;
		}
		rc = notification_from_stmt(stmt, notification);
		if (rc != 0)
			goto out_fail;
	}
	if (rc != SQLITE_DONE) {
		rc = MORPH_ERR_DB;
		goto out_fail;
	}
	sqlite3_finalize(stmt);
	if (arr.nelts > INT_MAX) {
		notification_free_list(arr.elts, (int)arr.nelts);
		MORPH_RETURN(-EOVERFLOW);
	}
	*out = arr.elts;
	*count = (int)arr.nelts;
	return 0;

out_fail:
	if (stmt)
		sqlite3_finalize(stmt);
	if (arr_ready)
		notification_free_list(arr.elts, (int)arr.nelts);
	MORPH_RETURN(rc);
}

int notification_mark_read(struct db *db, int64_t id, int64_t read_at)
{
	sqlite3_stmt *stmt = NULL;
	int rc;
	const char *sql = "UPDATE notifications SET read_at=? WHERE id=?";

	if (!db || !db->handle || id <= 0 || read_at < 0)
		MORPH_RETURN(-EINVAL);
	if (read_at == 0)
		read_at = (int64_t)time(NULL);
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, read_at);
	sqlite3_bind_int64(stmt, 2, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	if (sqlite3_changes(db->handle) == 0)
		MORPH_RETURN(-ENOENT);
	return 0;
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
