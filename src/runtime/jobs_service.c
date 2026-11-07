#include "runtime/runtime_internal.h"
#include "agent/tools/exec_tool.h"
#include "agent/tools/exec_jobs.h"
#include "util/strmap.h"
#include "util/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int runtime_process_event(const struct morph_event *event, void *user_data)
{
	struct runtime *runtime = user_data;
	const struct json *data = event->data;
	int64_t owner = (int64_t)json_number_value(json_member(data, "owner_session_id"));
	if (owner > 0) {
		char *json = json_stringify(data);
		sqlite3_stmt *stmt = NULL;
		pthread_mutex_lock(&runtime->jobs_lock);
		int rc = json ? sqlite3_prepare_v2(
					runtime->context.database.handle,
					"INSERT INTO "
					"process_jobs(process_id,session_id,data_json,updated_at) "
					"VALUES(?,?,?,strftime('%s','now')) ON "
					"CONFLICT(process_id) DO UPDATE "
					"SET "
					"data_json=excluded.data_json,updated_at=excluded.updated_"
					"at",
					-1, &stmt, NULL)
			      : SQLITE_NOMEM;
		if (rc == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, json_str_or(data, "session_id", ""), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_int64(stmt, 2, owner);
			sqlite3_bind_text(stmt, 3, json, -1, SQLITE_TRANSIENT);
			rc = sqlite3_step(stmt);
		}
		sqlite3_finalize(stmt);
		pthread_mutex_unlock(&runtime->jobs_lock);
		free(json);
		if (rc != SQLITE_DONE)
			log_warn(runtime->options.logger, "Could not retain process state: %s",
				 morph_strerror(MORPH_ERR_DB));
	}
	return morph_event_emit(runtime->options.event_cb, runtime->options.event_user_data, event);
}

int runtime_jobs_init(struct runtime *runtime)
{
	int rc = pthread_mutex_init(&runtime->jobs_lock, NULL);
	if (rc)
		return -rc;
	runtime->jobs_lock_ready = 1;
	rc = sqlite3_exec(
		runtime->context.database.handle,
		"CREATE TABLE IF NOT EXISTS process_jobs("
		"process_id TEXT PRIMARY KEY,session_id INTEGER NOT NULL "
		"REFERENCES sessions(id) ON DELETE CASCADE, data_json TEXT NOT NULL,"
		"updated_at INTEGER NOT NULL);"
		"CREATE INDEX IF NOT EXISTS process_jobs_session ON process_jobs(session_id);",
		NULL, NULL, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	runtime->jobs = exec_tool_jobs_handle(&runtime->context.tools);
	rc = exec_tool_set_event_callback(&runtime->context.tools, runtime_process_event, runtime);
	return rc == -ENOENT ? 0 : rc;
}

void runtime_jobs_cleanup(struct runtime *runtime)
{
	if (runtime->jobs_lock_ready) {
		pthread_mutex_destroy(&runtime->jobs_lock);
		runtime->jobs_lock_ready = 0;
	}
}

static void stored_job_unavailable(struct json *data)
{
	const char *status = json_str_or(data, "status", "");
	if (!strcmp(status, "running") || !strcmp(status, "starting")) {
		json_object_remove(data, "status");
		json_add_string(data, "status", "unavailable");
	}
	/* Spool files belong to the runtime that created the process. */
	json_object_remove(data, "stdout_path");
	json_object_remove(data, "stderr_path");
}

int runtime_process_list(struct runtime *runtime, struct json **out)
{
	if (!runtime || !out || !runtime->jobs_lock_ready)
		return -EINVAL;
	int64_t owner = runtime->context.current_session.id;
	struct json *jobs = NULL;
	int rc = runtime->jobs ? exec_jobs_list(runtime->jobs, owner, &jobs) : 0;
	if (!runtime->jobs) {
		jobs = json_array();
		if (!jobs)
			rc = -ENOMEM;
	}
	if (rc)
		return rc;
	morph_strmap_t live;
	rc = morph_strmap_init(&live, MORPH_STRMAP_INIT_CAP);
	if (rc) {
		json_free(jobs);
		return rc;
	}
	struct json *job;
	json_foreach (job, jobs) {
		rc = morph_strmap_set(&live, json_str_or(job, "session_id", ""), job);
		if (rc)
			break;
	}
	sqlite3_stmt *stmt = NULL;
	pthread_mutex_lock(&runtime->jobs_lock);
	int sql_rc = rc ? SQLITE_ERROR
			: sqlite3_prepare_v2(runtime->context.database.handle,
					     "SELECT data_json FROM process_jobs WHERE "
					     "session_id=? ORDER BY updated_at DESC "
					     "LIMIT 200",
					     -1, &stmt, NULL);
	if (sql_rc == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, owner);
		while ((sql_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
			const char *json = (const char *)sqlite3_column_text(stmt, 0);
			job = json ? json_parse_prefix(json, NULL, NULL) : NULL;
			if (!json_is_object_value(job)) {
				json_free(job);
				rc = MORPH_ERR_PARSE;
				break;
			}
			if (morph_strmap_contains(&live, json_str_or(job, "session_id", ""))) {
				json_free(job);
				continue;
			}
			stored_job_unavailable(job);
			if (json_array_add(jobs, job) != 0) {
				json_free(job);
				rc = -ENOMEM;
				break;
			}
		}
	}
	sqlite3_finalize(stmt);
	pthread_mutex_unlock(&runtime->jobs_lock);
	morph_strmap_cleanup(&live);
	if (!rc && sql_rc != SQLITE_DONE)
		rc = MORPH_ERR_DB;
	if (rc) {
		json_free(jobs);
		return rc;
	}
	*out = jobs;
	return 0;
}

static int resolve_job(struct runtime *runtime, const char *id, char **resolved)
{
	if (!id || !*id)
		return -EINVAL;
	if (!strncmp(id, "proc_", 5)) {
		*resolved = strdup(id);
		return *resolved ? 0 : -ENOMEM;
	}
	struct json *jobs = NULL, *job;
	int rc = runtime_process_list(runtime, &jobs);
	if (rc)
		return rc;
	const char *match = NULL;
	json_foreach (job, jobs) {
		const char *candidate = json_str_or(job, "session_id", "");
		const char *short_id = !strncmp(candidate, "proc_", 5) ? candidate + 5 : candidate;
		if (strcmp(candidate, id) && strncmp(short_id, id, strlen(id)))
			continue;
		if (match) {
			json_free(jobs);
			return -EINVAL;
		}
		match = candidate;
	}
	*resolved = match ? strdup(match) : NULL;
	rc = match ? (*resolved ? 0 : -ENOMEM) : -ENOENT;
	json_free(jobs);
	return rc;
}

int runtime_process_read(struct runtime *runtime, const char *id, struct json **out)
{
	char *resolved = NULL;
	int rc = resolve_job(runtime, id, &resolved);
	if (rc)
		return rc;
	rc = runtime->jobs
		     ? exec_jobs_read_retained(runtime->jobs, runtime->context.current_session.id,
					       resolved, out)
		     : -ENOENT;
	if (rc == -ENOENT) {
		sqlite3_stmt *stmt = NULL;
		pthread_mutex_lock(&runtime->jobs_lock);
		int sql_rc = sqlite3_prepare_v2(
			runtime->context.database.handle,
			"SELECT data_json FROM process_jobs WHERE process_id=? AND session_id=?",
			-1, &stmt, NULL);
		if (sql_rc == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, resolved, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int64(stmt, 2, runtime->context.current_session.id);
			sql_rc = sqlite3_step(stmt);
		}
		*out = sql_rc == SQLITE_ROW
			       ? json_parse_prefix((const char *)sqlite3_column_text(stmt, 0), NULL,
						   NULL)
			       : NULL;
		sqlite3_finalize(stmt);
		pthread_mutex_unlock(&runtime->jobs_lock);
		rc = *out		     ? 0
		     : sql_rc == SQLITE_DONE ? -ENOENT
		     : sql_rc == SQLITE_ROW  ? MORPH_ERR_PARSE
					     : MORPH_ERR_DB;
		if (*out)
			stored_job_unavailable(*out);
	}
	free(resolved);
	return rc;
}

int runtime_process_stop(struct runtime *runtime, const char *id)
{
	char *resolved = NULL;
	int rc = resolve_job(runtime, id, &resolved);
	if (!rc)
		rc = runtime->jobs ? exec_jobs_stop(runtime->jobs,
						    runtime->context.current_session.id, resolved)
				   : -ENOENT;
	free(resolved);
	return rc;
}
