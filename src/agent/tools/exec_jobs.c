#include "exec_jobs.h"
#include "util/array.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct exec_job {
	struct json *data;
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

static struct exec_job *find_job(struct exec_jobs *jobs, const char *id)
{
	struct exec_job *job;
	morph_array_foreach (job, &jobs->entries, struct exec_job) {
		if (!strcmp(json_str_or(job->data, "session_id", ""), id))
			return job;
	}
	return NULL;
}

static int job_owned(const struct json *data, int64_t owner)
{
	return json_number_value(json_member(data, "owner_session_id")) == (double)owner;
}

static void add_status(struct json *data, const struct process_snapshot *snapshot)
{
	json_object_remove(data, "status");
	json_object_remove(data, "duration_ms");
	json_object_remove(data, "exit_code");
	json_object_remove(data, "signal");
	json_add_string(data, "status", process_state_name(snapshot->state));
	json_add_number(data, "duration_ms", (double)snapshot->duration_ms);
	if (snapshot->state == PROCESS_EXITED)
		json_add_number(data, "exit_code", snapshot->exit_code);
	if (snapshot->signal_number)
		json_add_number(data, "signal", snapshot->signal_number);
}

static void emit_job(struct exec_jobs *jobs, const char *name, struct json *data)
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
		return -ENOMEM;
	int rc = pthread_mutex_init(&jobs->lock, NULL);
	if (rc) {
		free(jobs);
		return -rc;
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
	morph_array_foreach (job, &jobs->entries, struct exec_job)
		json_free(job->data);
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
	struct json *event = NULL;
	if (process_session_status(jobs->manager, session_id, &snapshot) != 0 ||
	    snapshot.state < PROCESS_EXITED)
		return;
	pthread_mutex_lock(&jobs->lock);
	struct exec_job *job = find_job(jobs, session_id);
	if (job && job->announced && !job->completed) {
		add_status(job->data, &snapshot);
		event = json_clone(job->data, 1);
		if (event)
			job->completed = 1;
	}
	pthread_mutex_unlock(&jobs->lock);
	if (event) {
		struct json *details = NULL;
		int64_t owner = (int64_t)json_number_value(json_member(event, "owner_session_id"));
		if (!exec_jobs_read(jobs, owner, session_id, &details)) {
			json_free(event);
			event = details;
		}
		emit_job(jobs, "process.completed", event);
		json_free(event);
	}
}

int exec_jobs_add(struct exec_jobs *jobs, const char *id, const char *command,
		  const char *description, const char *workdir, int64_t owner, const char *turn_id,
		  int service, int notify_only, int host, int network)
{
	struct json *data = json_object();
	char *safe_command = process_redact_text(jobs->manager, command);
	char *safe_description = process_redact_text(jobs->manager, description ? description : "");
	if (!data || !safe_command || !safe_description) {
		json_free(data);
		free(safe_command);
		free(safe_description);
		return -ENOMEM;
	}
	struct process_snapshot initial = {0};
	(void)process_session_status(jobs->manager, id, &initial);
	int ok = json_add_string(data, "session_id", id) &&
		 json_add_string(data, "command", safe_command) &&
		 json_add_string(data, "description", safe_description) &&
		 json_add_string(data, "workdir", workdir ? workdir : "") &&
		 json_add_string(data, "turn_id", turn_id ? turn_id : "") &&
		 json_add_number(data, "owner_session_id", (double)owner) &&
		 json_add_bool(data, "service", service) &&
		 json_add_bool(data, "notify_only", notify_only) &&
		 json_add_bool(data, "host", host) && json_add_bool(data, "network", network) &&
		 json_add_string(data, "status", "running") &&
		 json_add_number(data, "duration_ms", (double)initial.duration_ms);
	free(safe_command);
	free(safe_description);
	struct json *event = ok ? json_clone(data, 1) : NULL;
	if (!event) {
		json_free(data);
		return -ENOMEM;
	}
	pthread_mutex_lock(&jobs->lock);
	struct exec_job *job = morph_array_push(&jobs->entries);
	if (job)
		*job = (struct exec_job){.data = data};
	pthread_mutex_unlock(&jobs->lock);
	if (!job) {
		json_free(data);
		json_free(event);
		return -ENOMEM;
	}
	emit_job(jobs, "process.started", event);
	json_free(event);
	pthread_mutex_lock(&jobs->lock);
	find_job(jobs, id)->announced = 1;
	pthread_mutex_unlock(&jobs->lock);
	/* Covers exits before registration, including the start-notice race. */
	exec_jobs_complete(id, jobs);
	return 0;
}

int exec_jobs_list(struct exec_jobs *jobs, int64_t owner, struct json **out)
{
	struct json *array = json_array();
	int rc = 0;
	if (!array)
		return -ENOMEM;
	pthread_mutex_lock(&jobs->lock);
	struct exec_job *job;
	morph_array_foreach (job, &jobs->entries, struct exec_job) {
		struct process_snapshot snapshot;
		if (!job_owned(job->data, owner))
			continue;
		if (!process_session_status(jobs->manager, json_str_or(job->data, "session_id", ""),
					    &snapshot))
			add_status(job->data, &snapshot);
		struct json *data = json_clone(job->data, 1);
		if (!data || json_array_add(array, data) != 0) {
			json_free(data);
			rc = -ENOMEM;
			break;
		}
	}
	pthread_mutex_unlock(&jobs->lock);
	if (rc) {
		json_free(array);
		return rc;
	}
	*out = array;
	return 0;
}

static int read_job(struct exec_jobs *jobs, int64_t owner, const char *id, int full_output,
		    struct json **out)
{
	pthread_mutex_lock(&jobs->lock);
	struct exec_job *job = find_job(jobs, id);
	int owned = job && job_owned(job->data, owner);
	struct json *data = owned ? json_clone(job->data, 1) : NULL;
	pthread_mutex_unlock(&jobs->lock);
	if (!data)
		return owned ? -ENOMEM : -ENOENT;
	struct process_snapshot snapshot;
	int rc = process_session_inspect(jobs->manager, id, full_output, &snapshot);
	if (rc) {
		json_free(data);
		return rc;
	}
	add_status(data, &snapshot);
	json_add_string(data, "stdout", snapshot.stdout_text ? snapshot.stdout_text : "");
	json_add_string(data, "stderr", snapshot.stderr_text ? snapshot.stderr_text : "");
	json_add_string(data, "stdout_path", snapshot.stdout_path);
	json_add_string(data, "stderr_path", snapshot.stderr_path);
	json_add_bool(data, "stdout_truncated", snapshot.stdout_truncated);
	json_add_bool(data, "stderr_truncated", snapshot.stderr_truncated);
	process_snapshot_cleanup(&snapshot);
	*out = data;
	return 0;
}

int exec_jobs_read(struct exec_jobs *jobs, int64_t owner, const char *id, struct json **out)
{
	return read_job(jobs, owner, id, 0, out);
}

int exec_jobs_read_retained(struct exec_jobs *jobs, int64_t owner, const char *id,
			    struct json **out)
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
		   int (*cancelled)(void *), void *cancel_data, struct json **out)
{
	struct json *results = json_array();
	if (!results)
		return -ENOMEM;
	int rc = 0;
	for (;;) {
		char id[PROCESS_SESSION_ID_MAX] = {0};
		pthread_mutex_lock(&jobs->lock);
		struct exec_job *job;
		morph_array_foreach (job, &jobs->entries, struct exec_job) {
			if (job->observed || !job_owned(job->data, owner) ||
			    json_is_true(json_member(job->data, "service")) ||
			    json_is_true(json_member(job->data, "notify_only")) ||
			    strcmp(json_str_or(job->data, "turn_id", ""), turn_id ? turn_id : ""))
				continue;
			snprintf(id, sizeof(id), "%s", json_str_or(job->data, "session_id", ""));
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
		struct json *data = NULL;
		rc = exec_jobs_read(jobs, owner, id, &data);
		if (rc)
			break;
		if (json_array_add(results, data) != 0) {
			json_free(data);
			rc = -ENOMEM;
			break;
		}
		exec_jobs_observed(jobs, id);
	}
	if (rc == -ECANCELED && json_count(results))
		rc = 0;
	if (rc) {
		json_free(results);
		return rc;
	}
	*out = results;
	return 0;
}

void exec_jobs_cancel_turn(struct exec_jobs *jobs, int64_t owner, const char *turn_id)
{
	if (!jobs || !turn_id || !turn_id[0])
		return;
	pthread_mutex_lock(&jobs->lock);
	struct exec_job *job;
	morph_array_foreach (job, &jobs->entries, struct exec_job) {
		if (job_owned(job->data, owner) &&
		    !strcmp(json_str_or(job->data, "turn_id", ""), turn_id))
			(void)process_session_kill(jobs->manager,
						   json_str_or(job->data, "session_id", ""));
	}
	pthread_mutex_unlock(&jobs->lock);
}

int exec_jobs_authorize(struct exec_jobs *jobs, const char *id, int64_t owner)
{
	pthread_mutex_lock(&jobs->lock);
	struct exec_job *job = find_job(jobs, id);
	int allowed = !job || job_owned(job->data, owner);
	pthread_mutex_unlock(&jobs->lock);
	return allowed ? 0 : -ENOENT;
}

int exec_jobs_stop(struct exec_jobs *jobs, int64_t owner, const char *id)
{
	struct json *data = NULL;
	int rc = exec_jobs_read(jobs, owner, id, &data);
	if (!rc && strcmp(json_str_or(data, "status", ""), "running") &&
	    strcmp(json_str_or(data, "status", ""), "starting"))
		rc = -EALREADY;
	json_free(data);
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
	pthread_mutex_lock(&jobs->lock);
	morph_array_foreach (job, &jobs->entries, struct exec_job) {
		struct process_snapshot snapshot = {0};

		int rc = process_session_status(
			jobs->manager, json_str_or(job->data, "session_id", ""), &snapshot);
		if ((rc == 0 && snapshot.state < PROCESS_EXITED) || (rc != 0 && !job->completed))
			active = 1;
		process_snapshot_cleanup(&snapshot);
		if (active)
			break;
	}
	pthread_mutex_unlock(&jobs->lock);
	return active;
}

int exec_jobs_authority(struct exec_jobs *jobs, const char *id, int *host, int *network)
{
	struct exec_job *job;
	int rc = 0;

	if (!jobs || !id || !host || !network)
		return -EINVAL;
	pthread_mutex_lock(&jobs->lock);
	job = find_job(jobs, id);
	if (!job) {
		rc = -ENOENT;
	} else {
		*host = json_is_true(json_member(job->data, "host"));
		*network = json_is_true(json_member(job->data, "network"));
	}
	pthread_mutex_unlock(&jobs->lock);
	return rc;
}
