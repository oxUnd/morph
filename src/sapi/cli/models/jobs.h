#ifndef MORPH_CLI_MODELS_JOBS_H
#define MORPH_CLI_MODELS_JOBS_H

#include "cJSON.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include <stdint.h>

struct cli_jobs_job {
	cJSON *data;
	int64_t updated;
	int waiting;
};

struct cli_jobs {
	nb_vec entries;
	int64_t session_id;
};

int cli_jobs_model_create(struct cli_jobs **jobs);
void cli_jobs_model_cleanup(struct cli_jobs *jobs);
int cli_jobs_model_remember(struct cli_jobs *jobs, const cJSON *data);
void cli_jobs_model_set_session(struct cli_jobs *jobs, int64_t session_id);
int cli_jobs_model_visible(const struct cli_jobs *jobs);
int cli_jobs_model_waiting(const struct cli_jobs *jobs);
int cli_jobs_model_count(const struct cli_jobs *jobs);

int64_t cli_jobs_now(void);
const char *cli_jobs_job_string(const cJSON *data, const char *key);
const char *cli_jobs_job_short_id(const cJSON *data);
int cli_jobs_job_running(const cJSON *data);
int cli_jobs_job_cancelled(const cJSON *data);
int cli_jobs_job_failed(const cJSON *data);

#endif
