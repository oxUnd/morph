#include "jobs_service.h"
#include "persistence/orm/business_store.h"
#include <nb/err.h>
#include "runtime/runtime_internal.h"
#include "agent/tools/exec_tool.h"
#include "agent/tools/exec_jobs.h"
#include <nb/map.h>
#include <nb/log.h>
#include <nb/cleanup.h>
#include "cJSON.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *job_string(const cJSON *data, const char *key)
{
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, key));
	return text ? text : "";
}

static int64_t runtime_jobs_event_owner(const struct morph_event *event)
{
	if (!event || event->type != MORPH_EVENT_BACKGROUND || !event->name ||
	    (strcmp(event->name, "process.started") && strcmp(event->name, "process.completed")) ||
	    !cJSON_IsObject(event->data) || !job_string(event->data, "session_id")[0])
		return 0;
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(event->data, "owner_session_id");

	if (!cJSON_IsNumber(item))
		return 0;
	double number = cJSON_GetNumberValue(item);

	/* 2^63 is exactly representable; (double)INT64_MAX rounds up to it. */
	if (!isfinite(number) || number < 1.0 || number >= 0x1p63)
		return 0;
	int64_t owner = (int64_t)number;

	return (double)owner == number ? owner : 0;
}

int runtime_jobs_event(const struct morph_event *event, void *user_data)
{
	struct runtime *runtime = user_data;
	if (!runtime)
		return NB_ERR_INVAL;
	const cJSON *data = event ? event->data : NULL;
	int64_t owner = runtime_jobs_event_owner(event);
	if (owner > 0) {
		char *json = cJSON_PrintUnformatted(data);
		nb_mutex_lock(runtime->jobs_lock);
		int rc = json ? process_job_store_put(&runtime->context.database,
						      job_string(data, "session_id"), owner, json)
			      : NB_ERR_NOMEM;

		nb_mutex_unlock(runtime->jobs_lock);
		free(json);
		if (rc != 0)
			nb_log_write(runtime->options.logger, NB_LOG_WARN,
				     "Could not retain process state: %s",
				     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
						MORPH_ERR_DB));
	}
	return morph_event_emit(runtime->options.event_cb, runtime->options.event_user_data, event);
}

int runtime_jobs_init(struct runtime *runtime)
{
	int rc = ((runtime->jobs_lock = nb_mutex_new(0)) ? NB_OK : NB_ERR_NOMEM);
	if (rc)
		return rc;
	runtime->jobs_lock_ready = 1;
	rc = process_job_store_init(&runtime->context.database);
	if (rc != 0)
		return rc;

	rc = exec_tool_jobs_acquire(&runtime->context.tools, &runtime->jobs_owner);
	if (rc && rc != NB_ERR_NOT_FOUND)
		return rc;
	runtime->jobs = runtime->jobs_owner.jobs;
	rc = exec_tool_set_event_callback(&runtime->context.tools, runtime_jobs_event, runtime);
	return rc == NB_ERR_NOT_FOUND ? 0 : rc;
}

void runtime_jobs_cleanup(struct runtime *runtime)
{
	runtime->jobs = NULL;
	exec_tool_jobs_release(&runtime->jobs_owner);
	if (runtime->jobs_lock_ready) {
		(nb_mutex_free(runtime->jobs_lock), runtime->jobs_lock = NULL);
		runtime->jobs_lock_ready = 0;
	}
}

static void stored_job_unavailable(cJSON *data)
{
	const char *status = job_string(data, "status");
	if (!strcmp(status, "running") || !strcmp(status, "starting")) {
		cJSON_DeleteItemFromObjectCaseSensitive(data, "status");
		cJSON_AddStringToObject(data, "status", "unavailable");
	}
	/* Spool files belong to the runtime that created the process. */
	cJSON_DeleteItemFromObjectCaseSensitive(data, "stdout_path");
	cJSON_DeleteItemFromObjectCaseSensitive(data, "stderr_path");
}

int runtime_process_list(struct runtime *runtime, cJSON **out)
{
	if (!runtime || !out || !runtime->jobs_lock_ready)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	int64_t owner = runtime->context.current_session.id;
	cJSON *jobs = NULL;
	int rc = runtime->jobs ? exec_jobs_list(runtime->jobs, owner, &jobs) : 0;
	if (!runtime->jobs) {
		jobs = cJSON_CreateArray();
		if (!jobs)
			rc = NB_ERR_NOMEM;
	}
	if (rc)
		return rc;
	nb_map *live NB_CLEANUP(nb_map_cleanup) = nb_map_new_with_cap(16);
	if (!live) {
		cJSON_Delete(jobs);
		return NB_ERR_NOMEM;
	}
	cJSON *job;
	cJSON_ArrayForEach (job, jobs) {
		if (!nb_map_put_cstr(live, job_string(job, "session_id"), job, NULL)) {
			rc = NB_ERR_NOMEM;
			break;
		}
	}
	nb_vec records NB_CLEANUP(store_text_records_cleanup) = {0};

	nb_mutex_lock(runtime->jobs_lock);
	if (rc == 0)
		rc = process_job_store_list_copy(&runtime->context.database, owner, &records);

	for (size_t i = 0; rc == 0 && i < records.len; i++) {
		const char *json = *(char **)nb_vec_at(&records, i);

		job = json ? cJSON_Parse(json) : NULL;
		if (!cJSON_IsObject(job)) {
			cJSON_Delete(job);
			rc = NB_ERR_PARSE;
			break;
		}

		if (nb_map_has_cstr(live, job_string(job, "session_id"))) {
			cJSON_Delete(job);
			continue;
		}

		stored_job_unavailable(job);
		if (!cJSON_AddItemToArray(jobs, job)) {
			cJSON_Delete(job);
			rc = NB_ERR_NOMEM;
			break;
		}
	}

	nb_mutex_unlock(runtime->jobs_lock);
	if (rc) {
		cJSON_Delete(jobs);
		return rc;
	}
	*out = jobs;
	return 0;
}

static int resolve_job(struct runtime *runtime, const char *id, char **resolved)
{
	if (!id || !*id)
		return NB_ERR_INVAL;
	if (!strncmp(id, "proc_", 5)) {
		*resolved = strdup(id);
		return *resolved ? 0 : NB_ERR_NOMEM;
	}
	cJSON *jobs = NULL, *job;
	int rc = runtime_process_list(runtime, &jobs);
	if (rc)
		return rc;
	const char *match = NULL;
	cJSON_ArrayForEach (job, jobs) {
		const char *candidate = job_string(job, "session_id");
		const char *short_id = !strncmp(candidate, "proc_", 5) ? candidate + 5 : candidate;
		if (strcmp(candidate, id) && strncmp(short_id, id, strlen(id)))
			continue;
		if (match) {
			cJSON_Delete(jobs);
			return NB_ERR_INVAL;
		}
		match = candidate;
	}
	*resolved = match ? strdup(match) : NULL;
	rc = match ? (*resolved ? 0 : NB_ERR_NOMEM) : NB_ERR_NOT_FOUND;
	cJSON_Delete(jobs);
	return rc;
}

int runtime_process_read(struct runtime *runtime, const char *id, cJSON **out)
{
	char *resolved NB_CLEANUP(nb_charp_cleanup) = NULL;
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	if (!runtime || !out || !runtime->jobs_lock_ready)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	int rc = resolve_job(runtime, id, &resolved);
	if (rc)
		return rc;
	rc = runtime->jobs
		     ? exec_jobs_read_retained(runtime->jobs, runtime->context.current_session.id,
					       resolved, &result)
		     : NB_ERR_NOT_FOUND;
	if (rc == NB_ERR_NOT_FOUND) {
		char *json NB_CLEANUP(nb_charp_cleanup) = NULL;

		nb_mutex_lock(runtime->jobs_lock);
		rc = process_job_store_copy(&runtime->context.database,
					    runtime->context.current_session.id, resolved, &json);
		nb_mutex_unlock(runtime->jobs_lock);
		if (rc == 0) {
			result = json ? cJSON_Parse(json) : NULL;
			if (!result)
				rc = NB_ERR_PARSE;
		}

		if (result)
			stored_job_unavailable(result);
	}
	if (rc == 0) {
		*out = result;
		result = NULL;
	}
	return rc;
}

int runtime_process_stop(struct runtime *runtime, const char *id)
{
	char *resolved = NULL;
	int rc = resolve_job(runtime, id, &resolved);
	if (!rc)
		rc = runtime->jobs ? exec_jobs_stop(runtime->jobs,
						    runtime->context.current_session.id, resolved)
				   : NB_ERR_NOT_FOUND;
	free(resolved);
	return rc;
}
