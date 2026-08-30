#include "sapi/cli/internal.h"

#define CLI_COMMAND_POLL_TIMEOUT_MS 100

static void *cli_command_job_run(void *opaque)
{
	struct cli_command_job *job = opaque;
	int result;

	result = job->fn(job->ctx, job->input, job->user_data);
	pthread_mutex_lock(&job->mutex);
	job->result = result;
	job->done = 1;
	pthread_mutex_unlock(&job->mutex);
	cli_ui_notify(job->ctx);
	return NULL;
}

static int cli_command_handle(struct cli_context *ctx, const char *input,
			      void *user_data)
{
	(void)user_data;
	return cli_handle_command(ctx, input);
}

int cli_command_job_init(struct cli_command_job *job)
{
	int rc;

	if (!job)
		MORPH_RETURN(-EINVAL);
	memset(job, 0, sizeof(*job));
	rc = pthread_mutex_init(&job->mutex, NULL);
	if (rc != 0)
		MORPH_RETURN(-rc);
	return 0;
}

void cli_command_job_cleanup(struct cli_command_job *job)
{
	if (!job)
		return;
	if (job->active)
		(void)cli_command_job_finish(job);
	pthread_mutex_destroy(&job->mutex);
}

int cli_command_job_start(struct cli_command_job *job,
			  struct cli_context *ctx, const char *input)
{
	return cli_command_job_start_fn(job, ctx, input,
					cli_command_handle, NULL);
}

int cli_command_job_start_fn(struct cli_command_job *job,
			     struct cli_context *ctx, const char *input,
			     cli_command_job_fn fn, void *user_data)
{
	int thread_rc;

	if (!job || !ctx || !input || !fn || job->active)
		MORPH_RETURN(-EINVAL);
	job->input = strdup(input);
	if (!job->input)
		MORPH_RETURN(-ENOMEM);
	job->ctx = ctx;
	job->fn = fn;
	job->user_data = user_data;
	job->done = 0;
	job->result = 0;
	job->active = 1;
	thread_rc = pthread_create(&job->thread, NULL,
				   cli_command_job_run, job);
	if (thread_rc != 0) {
		job->active = 0;
		free(job->input);
		job->input = NULL;
		MORPH_RETURN(-thread_rc);
	}
	return 0;
}

int cli_command_job_done(struct cli_command_job *job)
{
	int done;

	if (!job || !job->active)
		return 0;
	pthread_mutex_lock(&job->mutex);
	done = job->done;
	pthread_mutex_unlock(&job->mutex);
	return done;
}

int cli_command_job_finish(struct cli_command_job *job)
{
	int result;

	if (!job || !job->active)
		MORPH_RETURN(-EINVAL);
	pthread_join(job->thread, NULL);
	result = job->result;
	free(job->input);
	job->input = NULL;
	job->fn = NULL;
	job->user_data = NULL;
	job->active = 0;
	job->done = 0;
	return result;
}

int cli_command_job_wait(struct cli_command_job *job)
{
	struct cli_context *ctx;
	int warned = 0;

	if (!job || !job->active || !job->ctx)
		MORPH_RETURN(-EINVAL);
	ctx = job->ctx;
	while (!cli_command_job_done(job)) {
		struct pollfd fd;
		int wake_fd = cli_ui_wake_fd(ctx);
		int nfds = wake_fd >= 0 ? 1 : 0;
		int rc;

		fd.fd = wake_fd;
		fd.events = POLLIN;
		fd.revents = 0;
		rc = poll(nfds > 0 ? &fd : NULL, (nfds_t)nfds,
			  CLI_COMMAND_POLL_TIMEOUT_MS);
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0 && !warned) {
			log_warn("command UI polling failed: %s", strerror(errno));
			warned = 1;
		}
		(void)cli_ui_drain(ctx);
	}
	(void)cli_ui_drain(ctx);
	return cli_command_job_finish(job);
}
