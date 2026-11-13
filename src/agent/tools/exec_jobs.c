#include "exec_jobs.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/cleanup.h"
#include "cJSON.h"

#include <errno.h>
#include "nb/thread.h"
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
	nb_mutex *lock;
	nb_vec entries;
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
	for (size_t _i_ = 0; _i_ < nb_vec_len(&jobs->entries) &&
			     (job = (struct exec_job *)nb_vec_at(&jobs->entries, _i_));
	     _i_++) {
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

static int add_status(cJSON *data, const struct process_snapshot *snapshot)
{
	cJSON_DeleteItemFromObjectCaseSensitive(data, "status");
	cJSON_DeleteItemFromObjectCaseSensitive(data, "duration_ms");
	cJSON_DeleteItemFromObjectCaseSensitive(data, "exit_code");
	cJSON_DeleteItemFromObjectCaseSensitive(data, "signal");
	if (!cJSON_AddStringToObject(data, "status", process_state_name(snapshot->state)) ||
	    !cJSON_AddNumberToObject(data, "duration_ms", (double)snapshot->duration_ms) ||
	    (snapshot->state == PROCESS_EXITED &&
	     !cJSON_AddNumberToObject(data, "exit_code", snapshot->exit_code)) ||
	    (snapshot->signal_number &&
	     !cJSON_AddNumberToObject(data, "signal", snapshot->signal_number)))
		return NB_ERR_NOMEM;
	return 0;
}

static void emit_job(struct exec_jobs *jobs, const char *name, cJSON *data)
{
	morph_event_cb cb;
	void *user_data;
	nb_mutex_lock(jobs->lock);
	cb = jobs->event_cb;
	user_data = jobs->event_user_data;
	nb_mutex_unlock(jobs->lock);
	(void)morph_event_emit_simple(cb, user_data, MORPH_EVENT_BACKGROUND, name,
				      !strcmp(name, "process.started") ||
				      !strcmp(name, "background.wait.begin") ? "begin" : "end", NULL,
				      data);
}

void exec_jobs_wait_event(struct exec_jobs *jobs, const char *id, int begin)
{
	cJSON *data NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

	if (!data || !cJSON_AddStringToObject(data, "session_id", id))
		return;
	emit_job(jobs, begin ? "background.wait.begin" : "background.wait.end", data);
}

int exec_jobs_create(struct exec_jobs **out)
{
	struct exec_job entry;
	struct exec_jobs *jobs;

	if (!out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_STATE;
	jobs = calloc(1, sizeof(*jobs));
	if (!jobs)
		return NB_ERR_NOMEM;
	int rc = ((jobs->lock = nb_mutex_new(0)) ? NB_OK : NB_ERR_NOMEM);
	if (rc) {
		free(jobs);
		return rc;
	}
	rc = (nb_vec_init(&jobs->entries, sizeof(entry)),
	      nb_vec_reserve(&jobs->entries, 8) ? 0 : NB_ERR_NOMEM);
	if (rc) {
		(nb_mutex_free(jobs->lock), jobs->lock = NULL);
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
	for (size_t _i_ = 0; _i_ < nb_vec_len(&jobs->entries) &&
			     (job = (struct exec_job *)nb_vec_at(&jobs->entries, _i_));
	     _i_++)
		cJSON_Delete(job->data);
	nb_vec_cleanup(&jobs->entries);
	(nb_mutex_free(jobs->lock), jobs->lock = NULL);
	free(jobs);
}

void exec_jobs_bind(struct exec_jobs *jobs, struct process_manager *manager, morph_event_cb cb,
		    void *user_data)
{
	nb_mutex_lock(jobs->lock);
	jobs->manager = manager;
	jobs->event_cb = cb;
	jobs->event_user_data = user_data;
	nb_mutex_unlock(jobs->lock);
}

void exec_jobs_complete(const char *session_id, void *user_data)
{
	struct exec_jobs *jobs = user_data;
	struct process_snapshot snapshot = {0};
	cJSON *event = NULL;
	if (process_session_status(jobs->manager, session_id, &snapshot) != 0 ||
	    snapshot.state < PROCESS_EXITED)
		return;
	nb_mutex_lock(jobs->lock);
	struct exec_job *job = find_job(jobs, session_id);
	if (job && job->announced && !job->completed) {
		(void)add_status(job->data, &snapshot);
		event = cJSON_Duplicate(job->data, 1);
		if (event)
			job->completed = 1;
	}
	nb_mutex_unlock(jobs->lock);
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
		  int service, int notify_only, int host, int network)
{
	cJSON *data = cJSON_CreateObject();
	char *safe_command = process_redact_text_copy(jobs->manager, command);
	char *safe_description =
		process_redact_text_copy(jobs->manager, description ? description : "");
	if (!data || !safe_command || !safe_description) {
		cJSON_Delete(data);
		free(safe_command);
		free(safe_description);
		return NB_ERR_NOMEM;
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
		 cJSON_AddBoolToObject(data, "host", host) &&
		 cJSON_AddBoolToObject(data, "network", network) &&
		 cJSON_AddStringToObject(data, "status", "running") &&
		 cJSON_AddNumberToObject(data, "duration_ms", (double)initial.duration_ms);
	free(safe_command);
	free(safe_description);
	cJSON *event = ok ? cJSON_Duplicate(data, 1) : NULL;
	if (!event) {
		cJSON_Delete(data);
		return NB_ERR_NOMEM;
	}
	nb_mutex_lock(jobs->lock);
	struct exec_job *job = nb_vec_push(&jobs->entries);
	if (job)
		*job = (struct exec_job){.data = data};
	nb_mutex_unlock(jobs->lock);
	if (!job) {
		cJSON_Delete(data);
		cJSON_Delete(event);
		return NB_ERR_NOMEM;
	}
	emit_job(jobs, "process.started", event);
	cJSON_Delete(event);
	nb_mutex_lock(jobs->lock);
	find_job(jobs, id)->announced = 1;
	nb_mutex_unlock(jobs->lock);
	/* Covers exits before registration, including the start-notice race. */
	exec_jobs_complete(id, jobs);
	return 0;
}

int exec_jobs_list(struct exec_jobs *jobs, int64_t owner, cJSON **out)
{
	cJSON *array;
	int rc = 0;

	if (!jobs || !out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_STATE;
	array = cJSON_CreateArray();
	if (!array)
		return NB_ERR_NOMEM;
	nb_mutex_lock(jobs->lock);
	struct exec_job *job;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&jobs->entries) &&
			     (job = (struct exec_job *)nb_vec_at(&jobs->entries, _i_));
	     _i_++) {
		struct process_snapshot snapshot = {0};
		if (!job_owned(job->data, owner))
			continue;
		int status_rc = process_session_status(
			jobs->manager, job_string(job->data, "session_id"), &snapshot);
		cJSON *data = cJSON_Duplicate(job->data, 1);
		if (!data || (status_rc == 0 && add_status(data, &snapshot) != 0) ||
		    !cJSON_AddItemToArray(array, data)) {
			cJSON_Delete(data);
			rc = NB_ERR_NOMEM;
			break;
		}
	}
	nb_mutex_unlock(jobs->lock);
	if (rc) {
		cJSON_Delete(array);
		return rc;
	}
	*out = array;
	return 0;
}

static int read_job(struct exec_jobs *jobs, int64_t owner, const char *id, int full_output,
		    cJSON **out)
{
	if (!jobs || !id || !out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_STATE;
	nb_mutex_lock(jobs->lock);
	struct exec_job *job = find_job(jobs, id);
	int owned = job && job_owned(job->data, owner);
	cJSON *data = owned ? cJSON_Duplicate(job->data, 1) : NULL;
	nb_mutex_unlock(jobs->lock);
	if (!data)
		return owned ? NB_ERR_NOMEM : NB_ERR_NOT_FOUND;
	struct process_snapshot snapshot = {0};
	int rc = process_session_inspect(jobs->manager, id, full_output, &snapshot);
	if (rc) {
		cJSON_Delete(data);
		return rc;
	}
	if (add_status(data, &snapshot) != 0 ||
	    !cJSON_AddStringToObject(data, "stdout",
				     snapshot.stdout_text ? snapshot.stdout_text : "") ||
	    !cJSON_AddStringToObject(data, "stderr",
				     snapshot.stderr_text ? snapshot.stderr_text : "") ||
	    !cJSON_AddStringToObject(data, "stdout_path", snapshot.stdout_path) ||
	    !cJSON_AddStringToObject(data, "stderr_path", snapshot.stderr_path) ||
	    !cJSON_AddNumberToObject(data, "stdout_total_bytes",
				     (double)snapshot.stdout_total_bytes) ||
	    !cJSON_AddNumberToObject(data, "stderr_total_bytes",
				     (double)snapshot.stderr_total_bytes) ||
	    !cJSON_AddNumberToObject(data, "stdout_path_bytes",
				     (double)snapshot.stdout_path_bytes) ||
	    !cJSON_AddNumberToObject(data, "stderr_path_bytes",
				     (double)snapshot.stderr_path_bytes) ||
	    !cJSON_AddNumberToObject(data, "stdout_retained_from",
				     (double)snapshot.stdout_retained_from) ||
	    !cJSON_AddNumberToObject(data, "stderr_retained_from",
				     (double)snapshot.stderr_retained_from) ||
	    !cJSON_AddNumberToObject(data, "stdout_unrecoverable_bytes",
				     (double)snapshot.stdout_unrecoverable_bytes) ||
	    !cJSON_AddNumberToObject(data, "stderr_unrecoverable_bytes",
				     (double)snapshot.stderr_unrecoverable_bytes) ||
	    !cJSON_AddBoolToObject(data, "stdout_path_truncated", snapshot.stdout_path_truncated) ||
	    !cJSON_AddBoolToObject(data, "stderr_path_truncated", snapshot.stderr_path_truncated) ||
	    (snapshot.stdout_capture_error &&
	     !cJSON_AddStringToObject(data, "stdout_capture_error",
				      nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
						 snapshot.stdout_capture_error))) ||
	    (snapshot.stderr_capture_error &&
	     !cJSON_AddStringToObject(data, "stderr_capture_error",
				      nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
						 snapshot.stderr_capture_error))) ||
	    !cJSON_AddBoolToObject(data, "stdout_truncated", snapshot.stdout_truncated) ||
	    !cJSON_AddBoolToObject(data, "stderr_truncated", snapshot.stderr_truncated)) {
		process_snapshot_cleanup(&snapshot);
		cJSON_Delete(data);
		return NB_ERR_NOMEM;
	}
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
	nb_mutex_lock(jobs->lock);
	struct exec_job *job = find_job(jobs, id);
	if (job)
		job->observed = 1;
	nb_mutex_unlock(jobs->lock);
}

int exec_jobs_wait(struct exec_jobs *jobs, int64_t owner, const char *turn_id,
		   int (*cancelled)(void *), void *cancel_data, cJSON **out)
{
	cJSON *results;

	if (!jobs || !out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_STATE;
	results = cJSON_CreateArray();
	if (!results)
		return NB_ERR_NOMEM;
	int rc = 0;
	for (;;) {
		char id[PROCESS_SESSION_ID_MAX] = {0};
		nb_mutex_lock(jobs->lock);
		struct exec_job *job;
		for (size_t _i_ = 0; _i_ < nb_vec_len(&jobs->entries) &&
				     (job = (struct exec_job *)nb_vec_at(&jobs->entries, _i_));
		     _i_++) {
			if (job->observed || !job_owned(job->data, owner) ||
			    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job->data, "service")) ||
			    cJSON_IsTrue(
				    cJSON_GetObjectItemCaseSensitive(job->data, "notify_only")) ||
			    strcmp(job_string(job->data, "turn_id"), turn_id ? turn_id : ""))
				continue;
			snprintf(id, sizeof(id), "%s", job_string(job->data, "session_id"));
			break;
		}
		nb_mutex_unlock(jobs->lock);
		if (!id[0])
			break;
		struct process_snapshot snapshot = {0};
		const struct process_cursor cursor = {0};
		exec_jobs_wait_event(jobs, id, 1);
		rc = process_session_wait(jobs->manager, id, &cursor, 30000, cancelled, cancel_data,
					  &snapshot);
		exec_jobs_wait_event(jobs, id, 0);
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
			rc = NB_ERR_NOMEM;
			break;
		}
		exec_jobs_observed(jobs, id);
	}
	if (rc == NB_ERR_CANCELLED && cJSON_GetArraySize(results))
		rc = 0;
	if (rc) {
		cJSON_Delete(results);
		return rc;
	}
	*out = results;
	return 0;
}

void exec_jobs_cancel_turn(struct exec_jobs *jobs, int64_t owner, const char *turn_id)
{
	if (!jobs || !turn_id || !turn_id[0])
		return;
	nb_mutex_lock(jobs->lock);
	struct exec_job *job;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&jobs->entries) &&
			     (job = (struct exec_job *)nb_vec_at(&jobs->entries, _i_));
	     _i_++) {
		if (job_owned(job->data, owner) &&
		    !strcmp(job_string(job->data, "turn_id"), turn_id))
			(void)process_session_kill(jobs->manager,
				job_string(job->data, "session_id"));
	}
	nb_mutex_unlock(jobs->lock);
}

int exec_jobs_authorize(struct exec_jobs *jobs, const char *id, int64_t owner)
{
	nb_mutex_lock(jobs->lock);
	struct exec_job *job = find_job(jobs, id);
	int allowed = !job || job_owned(job->data, owner);
	nb_mutex_unlock(jobs->lock);
	return allowed ? 0 : NB_ERR_NOT_FOUND;
}

int exec_jobs_stop(struct exec_jobs *jobs, int64_t owner, const char *id)
{
	cJSON *data = NULL;
	int rc = exec_jobs_read(jobs, owner, id, &data);
	if (!rc && strcmp(job_string(data, "status"), "running") &&
	    strcmp(job_string(data, "status"), "starting"))
		rc = NB_ERR_STATE;
	cJSON_Delete(data);
	if (rc)
		return rc;
	return process_session_kill(jobs->manager, id);
}

int exec_jobs_active(struct exec_jobs *jobs)
{
	struct exec_job *job;
	int active = 0;

	if (!jobs)
		return 0;
	nb_mutex_lock(jobs->lock);
	for (size_t _i_ = 0; _i_ < nb_vec_len(&jobs->entries) &&
			     (job = (struct exec_job *)nb_vec_at(&jobs->entries, _i_));
	     _i_++) {
		struct process_snapshot snapshot = {0};

		int rc = process_session_status(jobs->manager,
			job_string(job->data, "session_id"), &snapshot);
		if ((rc == 0 && snapshot.state < PROCESS_EXITED) || (rc != 0 && !job->completed))
			active = 1;
		process_snapshot_cleanup(&snapshot);
		if (active)
			break;
	}
	nb_mutex_unlock(jobs->lock);
	return active;
}

int exec_jobs_authority(struct exec_jobs *jobs, const char *id, int *host, int *network)
{
	struct exec_job *job;
	int rc = 0;

	if (!jobs || !id || !host || !network)
		return NB_ERR_INVAL;
	nb_mutex_lock(jobs->lock);
	job = find_job(jobs, id);
	if (!job) {
		rc = NB_ERR_NOT_FOUND;
	} else {
		*host = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job->data, "host"));
		*network = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job->data, "network"));
	}
	nb_mutex_unlock(jobs->lock);
	return rc;
}
