#include "sapi/cli/models/jobs.h"
#include <nb/err.h>
#include <nb/time.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int64_t cli_jobs_now(void)
{
	int64_t now = nb_time_monotonic_ns();

	return now > 0 ? now / 1000000 : 0;
}

const char *cli_jobs_job_string(const cJSON *data, const char *key)
{
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, key));

	return text ? text : "";
}

const char *cli_jobs_job_short_id(const cJSON *data)
{
	const char *id = cli_jobs_job_string(data, "session_id");

	return !strncmp(id, "proc_", 5) ? id + 5 : id;
}

int cli_jobs_job_running(const cJSON *data)
{
	const char *state = cli_jobs_job_string(data, "status");

	return !strcmp(state, "running") || !strcmp(state, "starting");
}

int cli_jobs_job_cancelled(const cJSON *data)
{
	const char *status = cli_jobs_job_string(data, "status");

	return !strcmp(status, "killed") || !strcmp(status, "cancelled") ||
	       !strcmp(status, "interrupted");
}

int cli_jobs_job_failed(const cJSON *data)
{
	return !cli_jobs_job_running(data) &&
	       (strcmp(cli_jobs_job_string(data, "status"), "exited") ||
		cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(data, "exit_code")) != 0);
}

int cli_jobs_model_create(struct cli_jobs **jobs)
{
	struct cli_jobs *created;
	int rc;

	if (!jobs || *jobs)
		return NB_ERR_INVAL;

	created = calloc(1, sizeof(*created));
	if (!created)
		return NB_ERR_NOMEM;

	rc = (nb_vec_init(&created->entries, sizeof(struct cli_jobs_job)),
	      nb_vec_reserve(&created->entries, 8) ? 0 : NB_ERR_NOMEM);
	if (rc) {
		free(created);
		return rc;
	}

	created->waiting_index = SIZE_MAX;
	*jobs = created;
	return 0;
}

void cli_jobs_model_cleanup(struct cli_jobs *jobs)
{
	struct cli_jobs_job *job;

	if (!jobs)
		return;

	for (size_t _i_ = 0; _i_ < nb_vec_len(&jobs->entries) &&
			     (job = (struct cli_jobs_job *)nb_vec_at(&jobs->entries, _i_));
	     _i_++)
		cJSON_Delete(job->data);

	nb_vec_cleanup(&jobs->entries);
	free(jobs);
}

/* Recompute only when the model mutates. An index survives vector growth,
 * including updates to a job outside the currently visible session. */
static void cli_jobs_model_changed(struct cli_jobs *jobs, const struct cli_jobs_job *updated)
{
	int running = 0;
	size_t waiting = SIZE_MAX;
	for (size_t i = 0; i < jobs->entries.len; i++) {
		const struct cli_jobs_job *job = nb_vec_at(&jobs->entries, i);
		if (cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
			    job->data, "owner_session_id")) != (double)jobs->session_id ||
		    !cli_jobs_job_running(job->data))
			continue;

		running++;
		if (job->waiting && waiting == SIZE_MAX)
			waiting = i;
	}

	if (running != jobs->running || waiting != jobs->waiting_index ||
	    (updated && waiting != SIZE_MAX && updated == nb_vec_at(&jobs->entries, waiting)))
		jobs->revision++;

	jobs->running = running;
	jobs->waiting_index = waiting;
}

void cli_jobs_model_set_session(struct cli_jobs *jobs, int64_t session_id)
{
	if (jobs && jobs->session_id != session_id) {
		jobs->session_id = session_id;
		cli_jobs_model_changed(jobs, NULL);
	}
}

int cli_jobs_model_remember(struct cli_jobs *jobs, const cJSON *data)
{
	struct cli_jobs_job *job;
	cJSON *copy;

	if (!jobs)
		return NB_ERR_INVAL;

	copy = cJSON_Duplicate(data, 1);
	if (!copy)
		return NB_ERR_NOMEM;

	for (size_t _i_ = 0; _i_ < nb_vec_len(&jobs->entries) &&
			     (job = (struct cli_jobs_job *)nb_vec_at(&jobs->entries, _i_));
	     _i_++) {
		if (!strcmp(cli_jobs_job_string(job->data, "session_id"),
			    cli_jobs_job_string(data, "session_id"))) {
			cJSON_Delete(job->data);
			job->data = copy;
			job->updated = cli_jobs_now();
			cli_jobs_model_changed(jobs, job);
			return 0;
		}
	}

	job = nb_vec_push(&jobs->entries);
	if (!job) {
		cJSON_Delete(copy);
		return NB_ERR_NOMEM;
	}

	*job = (struct cli_jobs_job){.data = copy, .updated = cli_jobs_now()};
	cli_jobs_model_changed(jobs, job);
	return 0;
}

void cli_jobs_model_note_wait(struct cli_jobs *jobs, const char *id, int begin)
{
	if (!jobs || !id)
		return;

	for (size_t i = 0; i < jobs->entries.len; i++) {
		struct cli_jobs_job *job = nb_vec_at(&jobs->entries, i);
		if (strcmp(cli_jobs_job_string(job->data, "session_id"), id))
			continue;

		if (begin)
			job->waiting++;
		else if (job->waiting)
			job->waiting--;
		else
			return;

		cli_jobs_model_changed(jobs, NULL);
		return;
	}
}

int64_t cli_jobs_model_wait_elapsed(const struct cli_jobs *jobs, int64_t now)
{
	const struct cli_jobs_job *job = cli_jobs_model_waiting(jobs);
	if (!job)
		return 0;

	const cJSON *duration = cJSON_GetObjectItemCaseSensitive(job->data, "duration_ms");
	return (cJSON_IsNumber(duration) ? (int64_t)duration->valuedouble : 0) +
	       (now - job->updated);
}

int cli_jobs_model_visible(const struct cli_jobs *jobs)
{
	return jobs && jobs->running > 0;
}

const struct cli_jobs_job *cli_jobs_model_waiting(const struct cli_jobs *jobs)
{
	return jobs && jobs->waiting_index != SIZE_MAX
		       ? nb_vec_at(&jobs->entries, jobs->waiting_index)
		       : NULL;
}

int cli_jobs_model_count(const struct cli_jobs *jobs)
{
	return jobs ? jobs->running : 0;
}
