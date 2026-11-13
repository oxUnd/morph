#include "sapi/cli/services/command_job.h"
#include "nb/err.h"
#include "sapi/cli/application.h"
#include "sapi/cli/services/turn.h"
#include "cJSON.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/application_internal.h"

#define CLI_COMMAND_JOB_COMMAND_POLL_TIMEOUT_MS 100

struct cli_command_job_prompt {
	char *text;
	char *payload;
	char *display_text;
};

static void *cli_command_job_run(void *opaque)
{
	struct cli_command_job *job = opaque;
	struct cli_application *ctx = job->ctx;
	(void)ctx;
	int result;

	result = job->fn(job->ctx, job->input, job->user_data);
	nb_mutex_lock(job->mutex);
	job->result = result;
	job->done = 1;
	nb_mutex_unlock(job->mutex);
	cli_event_queue_notify(job->ctx);
	return NULL;
}

static int cli_command_job_command_handle(struct cli_application *ctx, const char *input,
					  void *user_data)
{
	(void)user_data;
	return ctx->input_execute ? ctx->input_execute(ctx, input) : NB_ERR_UNSUPPORTED;
}

int cli_command_job_init(struct cli_command_job *job)
{
	struct cli_command_job_prompt prompt;
	int rc;

	if (!job)
		return NB_ERR_INVAL;
	memset(job, 0, sizeof(*job));
	rc = ((job->mutex = nb_mutex_new(0)) ? NB_OK : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = (nb_vec_init(&job->prompts, sizeof(prompt)),
	      nb_vec_reserve(&job->prompts, 16) ? 0 : NB_ERR_NOMEM);
	if (rc != 0) {
		(nb_mutex_free(job->mutex), job->mutex = NULL);
		return rc;
	}
	return 0;
}

void cli_command_job_cleanup(struct cli_command_job *job)
{
	if (!job)
		return;
	if (job->active)
		(void)cli_command_job_finish(job);
	for (size_t i = 0; i < job->prompts.len; i++) {
		struct cli_command_job_prompt *prompt = nb_vec_at(&job->prompts, i);

		free(prompt->text);
		free(prompt->payload);
		free(prompt->display_text);
	}
	nb_vec_cleanup(&job->prompts);
	free(job->delivered_prompt);
	(nb_mutex_free(job->mutex), job->mutex = NULL);
}

int cli_command_job_start(struct cli_command_job *job, struct cli_application *ctx,
			  const char *input)
{
	return cli_command_job_start_fn(job, ctx, input, cli_command_job_command_handle, NULL);
}

int cli_command_job_start_fn(struct cli_command_job *job, struct cli_application *ctx,
			     const char *input, cli_command_job_fn fn, void *user_data)
{
	int thread_rc;

	if (!job || !ctx || !input || !fn || job->active)
		return NB_ERR_INVAL;
	job->input = strdup(input);
	if (!job->input)
		return NB_ERR_NOMEM;
	job->ctx = ctx;
	job->fn = fn;
	job->user_data = user_data;
	job->done = 0;
	job->result = 0;
	job->active = 1;
	thread_rc = nb_thread_create(&job->thread, cli_command_job_run, job, NULL);
	if (thread_rc != 0) {
		job->active = 0;
		free(job->input);
		job->input = NULL;
		return thread_rc;
	}
	return 0;
}

int cli_command_job_done(struct cli_command_job *job)
{
	int done;

	if (!job || !job->active)
		return 0;
	nb_mutex_lock(job->mutex);
	done = job->done;
	nb_mutex_unlock(job->mutex);
	return done;
}

int cli_command_job_finish(struct cli_command_job *job)
{
	int result;

	if (!job || !job->active)
		return NB_ERR_INVAL;
	nb_thread_join_take(&job->thread, NULL);
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
	struct cli_application *ctx;
	int warned = 0;

	if (!job || !job->active || !job->ctx)
		return NB_ERR_INVAL;
	ctx = job->ctx;
	while (!cli_command_job_done(job)) {
		struct pollfd fd;
		int wake_fd = cli_event_queue_wake_fd(ctx);
		int nfds = wake_fd >= 0 ? 1 : 0;
		int rc;

		fd.fd = wake_fd;
		fd.events = POLLIN;
		fd.revents = 0;
		rc = poll(nfds > 0 ? &fd : NULL, (nfds_t)nfds,
			  CLI_COMMAND_JOB_COMMAND_POLL_TIMEOUT_MS);
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0 && !warned) {
			nb_log_write(ctx ? ctx->logger : NULL, NB_LOG_WARN,
				     "command UI polling failed: %s", strerror(errno));
			warned = 1;
		}
		(void)cli_event_queue_drain(ctx);
	}
	(void)cli_event_queue_drain(ctx);
	return cli_command_job_finish(job);
}

int cli_command_job_prompt(struct cli_command_job *job, const char *text, const char *display_text)
{
	cJSON *payload;
	char *json;
	struct cli_command_job_prompt prompt;
	struct cli_command_job_prompt *slot;
	int distinct_display;

	if (!job || !text || !text[0])
		return NB_ERR_INVAL;
	distinct_display = display_text && strcmp(display_text, text) != 0;
	payload = cJSON_CreateObject();
	if (!payload)
		return NB_ERR_NOMEM;
	if (!cJSON_AddStringToObject(payload, "text", text)) {
		cJSON_Delete(payload);
		return NB_ERR_NOMEM;
	}
	if (distinct_display && !cJSON_AddStringToObject(payload, "display_text", display_text)) {
		cJSON_Delete(payload);
		return NB_ERR_NOMEM;
	}
	json = cJSON_PrintUnformatted(payload);
	cJSON_Delete(payload);
	if (!json)
		return NB_ERR_NOMEM;
	prompt.text = strdup(text);
	prompt.payload = json;
	prompt.display_text = distinct_display ? strdup(display_text) : NULL;
	if (!prompt.text || (distinct_display && !prompt.display_text)) {
		free(prompt.text);
		free(prompt.display_text);
		free(json);
		return NB_ERR_NOMEM;
	}
	nb_mutex_lock(job->mutex);
	slot = nb_vec_push(&job->prompts);
	if (slot)
		*slot = prompt;
	nb_mutex_unlock(job->mutex);
	if (!slot) {
		free(prompt.text);
		free(prompt.display_text);
		free(json);
		return NB_ERR_NOMEM;
	}
	return 0;
}

int cli_command_job_prompts_copy(struct cli_command_job *job, char **items, size_t capacity,
				 size_t *total)
{
	size_t copied = 0;
	size_t available;

	if (!job || (!items && capacity > 0) || !total)
		return NB_ERR_INVAL;
	for (size_t i = 0; i < capacity; i++) {
		if (items[i])
			return NB_ERR_EXISTS;
	}
	*total = 0;
	nb_mutex_lock(job->mutex);
	available = job->prompts.len;
	while (copied < capacity && copied < available) {
		const struct cli_command_job_prompt *prompt = nb_vec_at(&job->prompts, copied);

		items[copied] = strdup(prompt->display_text ? prompt->display_text : prompt->text);
		if (!items[copied])
			break;
		copied++;
	}
	nb_mutex_unlock(job->mutex);
	if (copied < capacity && copied < available) {
		while (copied > 0) {
			copied--;
			free(items[copied]);
			items[copied] = NULL;
		}
		return NB_ERR_NOMEM;
	}
	*total = available;
	return 0;
}

/* The returned payload stays valid until the next drain, including when the
 * producer appends another prompt while ReAct is processing this one. */
int cli_command_job_drain(void *opaque, struct react_action *out, int timeout)
{
	struct cli_command_job *job = opaque;
	struct cli_application *ctx = job->ctx;
	(void)ctx;
	struct cli_command_job_prompt *items;

	(void)timeout;
	nb_mutex_lock(job->mutex);
	free(job->delivered_prompt);
	job->delivered_prompt = NULL;
	if (job->prompts.len > 0) {
		items = ((void *)job->prompts.data);
		job->delivered_prompt = items[0].payload;
		free(items[0].text);
		free(items[0].display_text);
		nb_vec_remove(&job->prompts, 0);
	}
	if (job->prompts.len == 0)
		job->expedite_prompts = 0;
	nb_mutex_unlock(job->mutex);
	out->type = "prompt";
	out->payload_json = job->delivered_prompt;
	if (out->payload_json && job->ctx)
		cli_event_queue_notify(job->ctx);
	return out->payload_json ? 1 : 0;
}

/* Called after joining the worker: submissions that missed the final drain
 * become the next turn instead of disappearing at the completion boundary. */
char *cli_command_job_take_prompt(struct cli_command_job *job, char **display_text)
{
	struct cli_command_job_prompt *items;
	char *text = NULL;

	if (display_text)
		*display_text = NULL;
	nb_mutex_lock(job->mutex);
	if (job->prompts.len > 0) {
		items = ((void *)job->prompts.data);
		text = items[0].text;
		if (display_text)
			*display_text = items[0].display_text;
		else
			free(items[0].display_text);
		free(items[0].payload);
		nb_vec_remove(&job->prompts, 0);
	}
	if (job->prompts.len == 0)
		job->expedite_prompts = 0;
	nb_mutex_unlock(job->mutex);
	return text;
}

int cli_command_job_expedite_prompts(struct cli_command_job *job)
{
	int pending;

	if (!job)
		return 0;
	nb_mutex_lock(job->mutex);
	pending = job->prompts.len > 0;
	job->expedite_prompts = pending;
	nb_mutex_unlock(job->mutex);
	return pending;
}

int cli_command_job_prompt_pending(void *opaque)
{
	struct cli_command_job *job = opaque;
	int pending;

	nb_mutex_lock(job->mutex);
	pending = job->expedite_prompts && job->prompts.len > 0;
	nb_mutex_unlock(job->mutex);
	return pending;
}
