#include "scheduled_task.h"
#include "util/array.h"
#include "util/error.h"
#include "util/log.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
	copy_text(task->title, sizeof(task->title),
		  (const char *)sqlite3_column_text(stmt, 1));
	copy_text(task->kind, sizeof(task->kind),
		  (const char *)sqlite3_column_text(stmt, 2));
	copy_text(task->status, sizeof(task->status),
		  (const char *)sqlite3_column_text(stmt, 3));
	copy_text(task->trigger_type, sizeof(task->trigger_type),
		  (const char *)sqlite3_column_text(stmt, 4));
	task->next_run_at = sqlite3_column_int64(stmt, 5);
	task->interval_seconds = sqlite3_column_int(stmt, 6);
	task->timeout_at = sqlite3_column_int64(stmt, 7);
	task->attempts = sqlite3_column_int(stmt, 8);
	task->max_attempts = sqlite3_column_int(stmt, 9);
	copy_text(task->action_type, sizeof(task->action_type),
		  (const char *)sqlite3_column_text(stmt, 10));
	task->payload_json = column_strdup(stmt, 11);
	task->policy_json = column_strdup(stmt, 12);
	task->notify_json = column_strdup(stmt, 13);
	copy_text(task->last_error, sizeof(task->last_error),
		  (const char *)sqlite3_column_text(stmt, 14));
	task->created_at = sqlite3_column_int64(stmt, 15);
	task->updated_at = sqlite3_column_int64(stmt, 16);

	if ((sqlite3_column_type(stmt, 11) != SQLITE_NULL && !task->payload_json) ||
	    (sqlite3_column_type(stmt, 12) != SQLITE_NULL && !task->policy_json) ||
	    (sqlite3_column_type(stmt, 13) != SQLITE_NULL && !task->notify_json)) {
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
	copy_text(notification->level, sizeof(notification->level),
		  (const char *)sqlite3_column_text(stmt, 2));
	copy_text(notification->title, sizeof(notification->title),
		  (const char *)sqlite3_column_text(stmt, 3));
	notification->body = column_strdup(stmt, 4);
	notification->created_at = sqlite3_column_int64(stmt, 5);
	notification->read_at = sqlite3_column_int64(stmt, 6);
	copy_text(notification->delivery_status,
		  sizeof(notification->delivery_status),
		  (const char *)sqlite3_column_text(stmt, 7));

	if (!notification->body)
		MORPH_RETURN(-ENOMEM);
	return 0;
}

int scheduled_task_create(struct db *db,
			  const struct scheduled_task_input *input,
			  struct scheduled_task *out)
{
	sqlite3_stmt *stmt = NULL;
	int64_t now;
	int rc;
	const char *sql =
		"INSERT INTO scheduled_tasks("
		"title,kind,status,trigger_type,next_run_at,interval_seconds,"
		"timeout_at,attempts,max_attempts,action_type,payload_json,"
		"policy_json,notify_json,last_error,created_at,updated_at)"
		" VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

	if (!db || !db->handle || !input || !input->title || !input->kind ||
	    !input->trigger_type || !input->action_type)
		MORPH_RETURN(-EINVAL);
	if (input->next_run_at < 0 || input->interval_seconds < 0 ||
	    input->timeout_at < 0 || input->max_attempts < 0)
		MORPH_RETURN(-EINVAL);

	now = (int64_t)time(NULL);
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);

	sqlite3_bind_text(stmt, 1, input->title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, input->kind, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, "pending", -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, input->trigger_type, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 5, input->next_run_at);
	sqlite3_bind_int(stmt, 6, input->interval_seconds);
	sqlite3_bind_int64(stmt, 7, input->timeout_at);
	sqlite3_bind_int(stmt, 8, 0);
	sqlite3_bind_int(stmt, 9, input->max_attempts);
	sqlite3_bind_text(stmt, 10, input->action_type, -1, SQLITE_TRANSIENT);
	if (input->payload_json)
		sqlite3_bind_text(stmt, 11, input->payload_json, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 11);
	if (input->policy_json)
		sqlite3_bind_text(stmt, 12, input->policy_json, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 12);
	if (input->notify_json)
		sqlite3_bind_text(stmt, 13, input->notify_json, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 13);
	sqlite3_bind_null(stmt, 14);
	sqlite3_bind_int64(stmt, 15, now);
	sqlite3_bind_int64(stmt, 16, now);

	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE) {
		log_err("scheduled task create failed: %s",
			sqlite3_errmsg(db->handle));
		MORPH_RETURN(MORPH_ERR_DB);
	}

	if (out)
		return scheduled_task_get(db, sqlite3_last_insert_rowid(db->handle),
					  out);
	return 0;
}

int scheduled_task_get(struct db *db, int64_t id, struct scheduled_task *out)
{
	sqlite3_stmt *stmt = NULL;
	int rc;
	const char *sql =
		"SELECT id,title,kind,status,trigger_type,next_run_at,"
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

int scheduled_task_list_due(struct db *db, int64_t now, int limit,
			    struct scheduled_task **out, int *count)
{
	sqlite3_stmt *stmt = NULL;
	morph_array_t arr;
	int arr_ready = 0;
	int bind_idx = 1;
	int rc;
	const char *sql =
		"SELECT id,title,kind,status,trigger_type,next_run_at,"
		"interval_seconds,timeout_at,attempts,max_attempts,action_type,"
		"payload_json,policy_json,notify_json,last_error,created_at,"
		"updated_at FROM scheduled_tasks "
		"WHERE status IN ('pending','waiting') AND next_run_at <= ? "
		"ORDER BY next_run_at ASC, id ASC LIMIT ?";

	if (!db || !db->handle || now < 0 || !out || !count)
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

int scheduled_task_update_run(struct db *db, int64_t id, const char *status,
			      int64_t next_run_at, int attempts,
			      const char *last_error)
{
	sqlite3_stmt *stmt = NULL;
	int64_t now;
	int rc;
	const char *sql =
		"UPDATE scheduled_tasks SET status=?,next_run_at=?,attempts=?,"
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
	if (last_error)
		sqlite3_bind_text(stmt, 4, last_error, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 4);
	sqlite3_bind_int64(stmt, 5, now);
	sqlite3_bind_int64(stmt, 6, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	if (sqlite3_changes(db->handle) == 0)
		MORPH_RETURN(-ENOENT);
	return 0;
}

int scheduled_task_cancel(struct db *db, int64_t id)
{
	sqlite3_stmt *stmt = NULL;
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
	return 0;
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

int notification_create(struct db *db, int64_t task_id, const char *level,
			const char *title, const char *body,
			const char *delivery_status,
			struct notification *out)
{
	sqlite3_stmt *stmt = NULL;
	int64_t now;
	int rc;
	const char *sql =
		"INSERT INTO notifications(task_id,level,title,body,created_at,"
		"read_at,delivery_status) VALUES(?,?,?,?,?,?,?)";

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
	sqlite3_bind_text(stmt, 2, level, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, body, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 5, now);
	sqlite3_bind_int64(stmt, 6, 0);
	sqlite3_bind_text(stmt, 7, delivery_status, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);

	if (out) {
		struct notification *list = NULL;
		int count = 0;

		rc = notification_list_unread(db, 1, &list, &count);
		if (rc != 0)
			return rc;
		if (count == 1) {
			*out = list[0];
			free(list);
		}
	}
	return 0;
}

int notification_list_unread(struct db *db, int limit,
			     struct notification **out, int *count)
{
	sqlite3_stmt *stmt = NULL;
	morph_array_t arr;
	int arr_ready = 0;
	int rc;
	const char *sql =
		"SELECT id,task_id,level,title,body,created_at,read_at,"
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
