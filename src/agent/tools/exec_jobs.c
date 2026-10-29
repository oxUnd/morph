#include "exec_jobs.h"
#include "util/array.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct exec_job {
	cJSON *data;
	int announced;
	int completed;
	int observed;
};

struct exec_jobs {
	pthread_mutex_t lock;
	morph_array_t entries;
	struct process_manager *manager;
	morph_event_cb event_cb;
	void *event_user_data;
};

static const char *job_string(const cJSON *data, const char *key)
{
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, key));
	return text ? text : "";
}

static struct exec_job *find_job(struct exec_jobs *jobs, const char *id)
{
	struct exec_job *job;
	morph_array_foreach(job, &jobs->entries, struct exec_job)
	{
		if (!strcmp(job_string(job->data, "session_id"), id))
			return job;
	}
	return NULL;
}

static int job_owned(const cJSON *data, int64_t owner)
{
	return cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(data, "owner_session_id")) ==
	       (double)owner;
}

static void add_status(cJSON *data, const struct process_snapshot *snapshot)
{
	cJSON_DeleteItemFromObjectCaseSensitive(data, "status");
	cJSON_DeleteItemFromObjectCaseSensitive(data, "duration_ms");
	cJSON_DeleteItemFromObjectCaseSensitive(data, "exit_code");
	cJSON_DeleteItemFromObjectCaseSensitive(data, "signal");
	cJSON_AddStringToObject(data, "status", process_state_name(snapshot->state));
	cJSON_AddNumberToObject(data, "duration_ms", (double)snapshot->duration_ms);
	if (snapshot->state == PROCESS_EXITED)
		cJSON_AddNumberToObject(data, "exit_code", snapshot->exit_code);
	if (snapshot->signal_number)
		cJSON_AddNumberToObject(data, "signal", snapshot->signal_number);
}

static void emit_job(struct exec_jobs *jobs, const char *name, cJSON *data)
{
	morph_event_cb cb;
	void *user_data;
	pthread_mutex_lock(&jobs->lock);
	cb = jobs->event_cb;
	user_data = jobs->event_user_data;
	pthread_mutex_unlock(&jobs->lock);
	(void)morph_event_emit_simple(cb, user_data, MORPH_EVENT_BACKGROUND, name,
				      !strcmp(name, "process.started") ? "begin" : "end", NULL,
				      data);
}

int exec_jobs_create(struct exec_jobs **out)
{
	struct exec_job entry;
	struct exec_jobs *jobs = calloc(1, sizeof(*jobs));
	if (!jobs)
		MORPH_RETURN(-ENOMEM);
	int rc = pthread_mutex_init(&jobs->lock, NULL);
	if (rc) {
		free(jobs);
		MORPH_RETURN(-rc);
	}
	rc = morph_array_init(&jobs->entries, 8, sizeof(entry));
	if (rc) {
		pthread_mutex_destroy(&jobs->lock);
		free(jobs);
		return rc;
	}
	*out = jobs;
	return 0;
}

void exec_jobs_destroy(struct exec_jobs *jobs)
{
	if (!jobs)
		return;
	struct exec_job *job;
	morph_array_foreach(job, &jobs->entries, struct exec_job) cJSON_Delete(job->data);
	morph_array_cleanup(&jobs->entries);
	pthread_mutex_destroy(&jobs->lock);
	free(jobs);
}

void exec_jobs_bind(struct exec_jobs *jobs, struct process_manager *manager, morph_event_cb cb,
		    void *user_data)
{
	pthread_mutex_lock(&jobs->lock);
	jobs->manager = manager;
	jobs->event_cb = cb;
	jobs->event_user_data = user_data;
	pthread_mutex_unlock(&jobs->lock);
}

void exec_jobs_complete(const char *session_id, void *user_data)
{
	struct exec_jobs *jobs = user_data;
	struct process_snapshot snapshot;
	cJSON *event = NULL;
	if (process_session_status(jobs->manager, session_id, &snapshot) != 0 ||
	    snapshot.state < PROCESS_EXITED)
		return;
	pthread_mutex_lock(&jobs->lock);
	struct exec_job *job = find_job(jobs, session_id);
	if (job && job->announced && !job->completed) {
		add_status(job->data, &snapshot);
		event = cJSON_Duplicate(job->data, 1);
		if (event)
			job->completed = 1;
	}
	pthread_mutex_unlock(&jobs->lock);
	if (event) {
		cJSON *details = NULL;
		int64_t owner = (int64_t)cJSON_GetNumberValue(
			cJSON_GetObjectItemCaseSensitive(event, "owner_session_id"));
		if (!exec_jobs_read(jobs, owner, session_id, &details)) {
			cJSON_Delete(event);
			event = details;
		}
		emit_job(jobs, "process.completed", event);
		cJSON_Delete(event);
	}
}

int exec_jobs_add(struct exec_jobs *jobs, const char *id, const char *command,
		  const char *description, const char *workdir, int64_t owner, const char *turn_id,
		  int service, int notify_only)
{
	cJSON *data = cJSON_CreateObject();
	char *safe_command = process_redact_text(jobs->manager, command);
	char *safe_description = process_redact_text(jobs->manager, description ? description : "");
	if (!data || !safe_command || !safe_description) {
		cJSON_Delete(data);
		free(safe_command);
		free(safe_description);
		MORPH_RETURN(-ENOMEM);
	}
	struct process_snapshot initial = {0};
	(void)process_session_status(jobs->manager, id, &initial);
	int ok = cJSON_AddStringToObject(data, "session_id", id) &&
		 cJSON_AddStringToObject(data, "command", safe_command) &&
		 cJSON_AddStringToObject(data, "description", safe_description) &&
		 cJSON_AddStringToObject(data, "workdir", workdir ? workdir : "") &&
		 cJSON_AddStringToObject(data, "turn_id", turn_id ? turn_id : "") &&
		 cJSON_AddNumberToObject(data, "owner_session_id", (double)owner) &&
		 cJSON_AddBoolToObject(data, "service", service) &&
		 cJSON_AddBoolToObject(data, "notify_only", notify_only) &&
		 cJSON_AddStringToObject(data, "status", "running") &&
		 cJSON_AddNumberToObject(data, "duration_ms", (double)initial.duration_ms);
	free(safe_command);
	free(safe_description);
	cJSON *event = ok ? cJSON_Duplicate(data, 1) : NULL;
	if (!event) {
		cJSON_Delete(data);
		MORPH_RETURN(-ENOMEM);
	}
	pthread_mutex_lock(&jobs->lock);
	struct exec_job *job = morph_array_push(&jobs->entries);
	if (job)
		*job = (struct exec_job){.data = data};
	pthread_mutex_unlock(&jobs->lock);
	if (!job) {
		cJSON_Delete(data);
		cJSON_Delete(event);
		MORPH_RETURN(-ENOMEM);
	}
	emit_job(jobs, "process.started", event);
	cJSON_Delete(event);
	pthread_mutex_lock(&jobs->lock);
	find_job(jobs, id)->announced = 1;
	pthread_mutex_unlock(&jobs->lock);
	/* Covers exits before registration, including the start-notice race. */
	exec_jobs_complete(id, jobs);
	return 0;
}

int exec_jobs_list(struct exec_jobs *jobs, int64_t owner, cJSON **out)
{
	cJSON *array = cJSON_CreateArray();
	int rc = 0;
	if (!array)
		MORPH_RETURN(-ENOMEM);
	pthread_mutex_lock(&jobs->lock);
	struct exec_job *job;
	morph_array_foreach(job, &jobs->entries, struct exec_job)
	{
		struct process_snapshot snapshot;
		if (!job_owned(job->data, owner))
			continue;
		if (!process_session_status(jobs->manager, job_string(job->data, "session_id"),
					    &snapshot))
			add_status(job->data, &snapshot);
		cJSON *data = cJSON_Duplicate(job->data, 1);
		if (!data || !cJSON_AddItemToArray(array, data)) {
			cJSON_Delete(data);
			rc = -ENOMEM;
			break;
		}
	}
	pthread_mutex_unlock(&jobs->lock);
	if (rc) {
		cJSON_Delete(array);
		MORPH_RETURN(rc);
	}
	*out = array;
	return 0;
}

static int read_job(struct exec_jobs *jobs, int64_t owner, const char *id, int full_output,
		    cJSON **out)
{
	pthread_mutex_lock(&jobs->lock);
	struct exec_job *job = find_job(jobs, id);
	int owned = job && job_owned(job->data, owner);
	cJSON *data = owned ? cJSON_Duplicate(job->data, 1) : NULL;
	pthread_mutex_unlock(&jobs->lock);
	if (!data)
		MORPH_RETURN(owned ? -ENOMEM : -ENOENT);
	struct process_snapshot snapshot;
	int rc = process_session_inspect(jobs->manager, id, full_output, &snapshot);
	if (rc) {
		cJSON_Delete(data);
		return rc;
	}
	add_status(data, &snapshot);
	cJSON_AddStringToObject(data, "stdout", snapshot.stdout_text ? snapshot.stdout_text : "");
	cJSON_AddStringToObject(data, "stderr", snapshot.stderr_text ? snapshot.stderr_text : "");
	cJSON_AddStringToObject(data, "stdout_path", snapshot.stdout_path);
	cJSON_AddStringToObject(data, "stderr_path", snapshot.stderr_path);
	cJSON_AddBoolToObject(data, "stdout_truncated", snapshot.stdout_truncated);
	cJSON_AddBoolToObject(data, "stderr_truncated", snapshot.stderr_truncated);
	process_snapshot_cleanup(&snapshot);
	*out = data;
	return 0;
}

int exec_jobs_read(struct exec_jobs *jobs, int64_t owner, const char *id, cJSON **out)
{
	return read_job(jobs, owner, id, 0, out);
}

int exec_jobs_read_retained(struct exec_jobs *jobs, int64_t owner, const char *id, cJSON **out)
{
	return read_job(jobs, owner, id, 1, out);
}

void exec_jobs_observed(struct exec_jobs *jobs, const char *id)
{
	pthread_mutex_lock(&jobs->lock);
	struct exec_job *job = find_job(jobs, id);
	if (job)
		job->observed = 1;
	pthread_mutex_unlock(&jobs->lock);
}

int exec_jobs_wait(struct exec_jobs *jobs, int64_t owner, const char *turn_id,
		   int (*cancelled)(void *), void *cancel_data, cJSON **out)
{
	cJSON *results = cJSON_CreateArray();
	if (!results)
		MORPH_RETURN(-ENOMEM);
	int rc = 0;
	for (;;) {
		char id[PROCESS_SESSION_ID_MAX] = {0};
		pthread_mutex_lock(&jobs->lock);
		struct exec_job *job;
		morph_array_foreach(job, &jobs->entries, struct exec_job)
		{
			if (job->observed || !job_owned(job->data, owner) ||
			    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job->data, "service")) ||
			    cJSON_IsTrue(
				    cJSON_GetObjectItemCaseSensitive(job->data, "notify_only")) ||
			    strcmp(job_string(job->data, "turn_id"), turn_id ? turn_id : ""))
				continue;
			snprintf(id, sizeof(id), "%s", job_string(job->data, "session_id"));
			break;
		}
		pthread_mutex_unlock(&jobs->lock);
		if (!id[0])
			break;
		struct process_snapshot snapshot = {0};
		const struct process_cursor cursor = {0};
		rc = process_session_wait(jobs->manager, id, &cursor, 30000, cancelled, cancel_data,
					  &snapshot);
		int ended = rc == 0 && snapshot.state >= PROCESS_EXITED;
		process_snapshot_cleanup(&snapshot);
		if (rc)
			break;
		if (!ended)
			continue;
		cJSON *data = NULL;
		rc = exec_jobs_read(jobs, owner, id, &data);
		if (rc)
			break;
		if (!cJSON_AddItemToArray(results, data)) {
			cJSON_Delete(data);
			rc = -ENOMEM;
			break;
		}
		exec_jobs_observed(jobs, id);
	}
	if (rc == -ECANCELED && cJSON_GetArraySize(results))
		rc = 0;
	if (rc) {
		cJSON_Delete(results);
		MORPH_RETURN(rc);
	}
	*out = results;
	return 0;
}

int exec_jobs_authorize(struct exec_jobs *jobs, const char *id, int64_t owner)
{
	pthread_mutex_lock(&jobs->lock);
	struct exec_job *job = find_job(jobs, id);
	int allowed = !job || job_owned(job->data, owner);
	pthread_mutex_unlock(&jobs->lock);
	MORPH_RETURN(allowed ? 0 : -ENOENT);
}

int exec_jobs_stop(struct exec_jobs *jobs, int64_t owner, const char *id)
{
	cJSON *data = NULL;
	int rc = exec_jobs_read(jobs, owner, id, &data);
	if (!rc && strcmp(job_string(data, "status"), "running") &&
	    strcmp(job_string(data, "status"), "starting"))
		rc = -EALREADY;
	cJSON_Delete(data);
	if (rc)
		return rc;
	return process_session_kill(jobs->manager, id);
}
