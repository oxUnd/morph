#ifndef MORPH_CLI_SERVICES_COMMAND_JOB_H
#define MORPH_CLI_SERVICES_COMMAND_JOB_H

#include "nb/thread.h"
#include "nb/err.h"
#include "nb/vec.h"
#include "nb/cleanup.h"

struct react_action;

struct cli_application;
typedef int (*cli_command_job_fn)(struct cli_application *ctx, const char *input, void *user_data);

struct cli_command_job {
	struct cli_application *ctx;
	char *input;
	cli_command_job_fn fn;
	void *user_data;
	nb_thread *thread;
	nb_mutex *mutex;
	int active;
	int done;
	int result;
	nb_vec prompts;
	char *delivered_prompt;
	int expedite_prompts;
};

int cli_command_job_init(struct cli_command_job *job);
void cli_command_job_cleanup(struct cli_command_job *job);
int cli_command_job_start(struct cli_command_job *job, struct cli_application *ctx,
			  const char *input);
int cli_command_job_start_fn(struct cli_command_job *job, struct cli_application *ctx,
			     const char *input, cli_command_job_fn fn, void *user_data);
int cli_command_job_done(struct cli_command_job *job);
int cli_command_job_finish(struct cli_command_job *job);
/* Only expedited prompts interrupt in-flight work. */
int cli_command_job_prompt_pending(void *opaque);
int cli_command_job_expedite_prompts(struct cli_command_job *job);
int cli_command_job_prompt(struct cli_command_job *job, const char *text, const char *display_text);
/* Each empty items slot receives an independent malloc-owned string. On failure
 * every created slot is NULL and total is zero; occupied slots return NB_ERR_EXISTS. */
int cli_command_job_prompts_copy(struct cli_command_job *job, char **items, size_t capacity,
				 size_t *total);
char *cli_command_job_take_prompt(struct cli_command_job *job, char **display_text);
int cli_command_job_drain(void *opaque, struct react_action *out, int timeout);
int cli_command_job_wait(struct cli_command_job *job);

#endif
