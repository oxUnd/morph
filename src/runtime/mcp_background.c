#include "runtime/runtime_internal.h"
#include "runtime/bootstrap.h"
#include "runtime/mcp.h"
#include "errors.h"
#include "nb/log.h"
#include "nb/time.h"

#include <errno.h>
#include <string.h>
#include <time.h>

int64_t runtime_mcp_now_ms(void)
{
	int64_t now = nb_time_monotonic_ns();

	return now > 0 ? now / 1000000 : 0;
}

struct runtime_mcp_job *runtime_mcp_job_find(struct runtime *runtime,
	struct mcp_client *client)
{
	for (int i = 0; i < runtime->mcp_job_count; i++) {
		if (runtime->mcp_jobs[i].client == client)
			return &runtime->mcp_jobs[i];
	}
	return NULL;
}

static int job_interrupted(void *opaque)
{
	struct runtime_mcp_job *job = opaque;
	int cancelled;
	enum runtime_mcp_state state;

	nb_mutex_lock(job->lock);
	cancelled = nb_cancel_requested(&job->cancel);
	state = job->state;
	nb_mutex_unlock(job->lock);
	if (cancelled)
		return 1;
	return state != RUNTIME_MCP_REGISTERING &&
		job->client->config.connect_timeout > 0 &&
		runtime_mcp_now_ms() - job->started_ms >=
		(int64_t)job->client->config.connect_timeout * 1000;
}

static void job_state(struct runtime_mcp_job *job, enum runtime_mcp_state state)
{
	nb_mutex_lock(job->lock);
	job->state = state;
	nb_mutex_unlock(job->lock);
}

static int job_event(const struct morph_event *event, void *opaque);

/** \brief Acquire an owner lock without preventing background job cancellation. */
static int job_lock(struct runtime_mcp_job *job, nb_mutex *mutex)
{
	struct timespec pause = {.tv_nsec = 10000000};
	for (;;) {
		if (job_interrupted(job)) {
			return NB_ERR_CANCELLED;
		}
		int rc = nb_mutex_trylock(mutex);
		if (rc == 0) {
			return 0;
		}
		if (rc != NB_ERR_BUSY) {
			return rc;
		}
		nanosleep(&pause, NULL);
	}
}

/* Transfer ownership only between turns. Discovery never touches live tools. */
static int job_publish(struct runtime_mcp_job *job, struct tool_registry *pending)
{
	struct runtime_context *ctx = &job->runtime->context;
	int rc;
	int added = 0;

	job_state(job, RUNTIME_MCP_REGISTERING);
	runtime_mcp_emit(job_event, job, "mcp.registering", "pending",
		"Waiting to register MCP capabilities between turns",
		&job->client->config, -1, -1, -1, 0);
	rc = job_lock(job, ctx->execution_lock);
	if (rc != 0) {
		return nb_err_trace(job->runtime->options.logger, rc, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	}
	rc = job_lock(job, ctx->tools.mutation_mutex);
	if (rc != 0) {
		nb_mutex_unlock(ctx->execution_lock);
		return nb_err_trace(job->runtime->options.logger, rc, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	}
	nb_mutex_lock(ctx->tool_registry_lock);
	/* Background sub-agents can still mutate the parent registry between turns.
	 * Keep lookup/copy publication exclusive, with mutation before the data lock. */
	tool_registry_lock(&ctx->tools);
	for (int i = 0; i < pending->count; i++) {
		struct tool_entry *entry = &pending->entries[i];
		struct tool_spec spec = {
			.origin = entry->origin,
			.name = entry->desc.name,
			.title = entry->desc.title,
			.description = entry->desc.description,
			.input_schema = entry->desc.input_schema,
			.output_schema = entry->desc.output_schema,
			.input_kind = entry->desc.input_kind,
			.input_format = entry->desc.input_format,
			.exec = entry->exec,
			.user_data = entry->user_data,
			.user_data_destroy = entry->user_data_destroy,
			.flags = entry->flags,
			.timeout_seconds = entry->timeout_seconds,
		};

		if (tool_borrow(&ctx->tools, spec.name)) {
			rc = NB_ERR_EXISTS;
			break;
		}
		rc = tool_register_copy(&ctx->tools, entry);
		if (rc != 0)
			break;
		added++;
	}
	if (rc != 0) {
		for (int i = 0; i < added; i++)
			(void)tool_unregister(&ctx->tools, pending->entries[i].desc.name);
	}
	if (rc == 0)
		runtime_bootstrap_tool_flags(&ctx->tools, &ctx->config);
	tool_registry_unlock(&ctx->tools);
	nb_mutex_unlock(ctx->tool_registry_lock);
	nb_mutex_unlock(ctx->tools.mutation_mutex);
	nb_mutex_unlock(ctx->execution_lock);
	return rc;
}

static int job_event(const struct morph_event *event, void *opaque)
{
	struct runtime_mcp_job *job = opaque;
	struct runtime *runtime = job->runtime;

	if (event->data)
		cJSON_AddBoolToObject(event->data, "background", 1);
	return runtime->options.event_cb ? runtime->options.event_cb(event,
		runtime->options.event_user_data) : 0;
}

static void *job_run(void *opaque)
{
	struct runtime_mcp_job *job = opaque;
	struct tool_registry pending;
	int t = 0, r = 0, p = 0;
	int rc;

	tool_registry_init(job->runtime->options.logger, &pending);
	job->client->interrupt_check = job_interrupted;
	job->client->interrupt_user_data = job;
	rc = runtime_mcp_client_connect(job->client, job_event, job);
	if (rc == 0) {
		job_state(job, RUNTIME_MCP_DISCOVERING);
		runtime_mcp_emit(job_event, job,
			"mcp.discovering", "begin", "Discovering MCP capabilities",
			&job->client->config, -1, -1, -1, 0);
		t = mcp_register_server_tools(job->client, &pending);
		if (t >= 0)
			r = mcp_register_server_resources(job->client, &pending);
		if (t >= 0 && r >= 0)
			p = mcp_register_server_prompts(job->client, &pending);
		rc = t < 0 ? t : (r < 0 ? r : (p < 0 ? p : 0));
		if (rc == 0)
			rc = job_publish(job, &pending);
	}
	job->client->interrupt_check = NULL;
	job->client->interrupt_user_data = NULL;
	tool_registry_cleanup(&pending);
	if (rc != 0)
		mcp_disconnect(job->client);
	nb_mutex_lock(job->lock);
	if (rc != 0 && nb_cancel_requested(&job->cancel))
		rc = NB_ERR_CANCELLED;
	else if (rc != 0 && job->client->config.connect_timeout > 0 &&
		 runtime_mcp_now_ms() - job->started_ms >=
		 (int64_t)job->client->config.connect_timeout * 1000)
		rc = NB_ERR_TIMEOUT;
	job->client->discovery_error = rc;
	job->client->capabilities_registered = rc == 0;
	job->error_code = rc;
	job->elapsed_ms = runtime_mcp_now_ms() - job->started_ms;
	job->state = rc == 0 ? RUNTIME_MCP_READY : RUNTIME_MCP_FAILED;
	job->active = 0;
	nb_mutex_unlock(job->lock);
	runtime_mcp_emit(job_event, job,
		rc == 0 ? "mcp.ready" : "mcp.failed",
		rc == 0 ? "ready" : "failed", "MCP initialization finished",
		&job->client->config, t, r, p, rc);
	return NULL;
}

int runtime_mcp_background_start(struct runtime *runtime)
{
	for (int i = 0; i < runtime->context.mcp.count; i++) {
		struct mcp_client *client = runtime->context.mcp.servers[i];
		struct runtime_mcp_job *job;
		int rc;

		if (!client->config.auto_connect)
			continue;
		job = &runtime->mcp_jobs[runtime->mcp_job_count];
		rc = ((job->lock = nb_mutex_new(0)) ? NB_OK : NB_ERR_NOMEM);
		if (rc != 0) {
			nb_log_write(job->runtime->options.logger, NB_LOG_WARN,
				     "mcp: worker lock failed: %s",
				     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
			continue;
		}
		job->runtime = runtime;
		job->client = client;
		job->state = RUNTIME_MCP_CONNECTING;
		job->active = 1;
		job->started_ms = runtime_mcp_now_ms();
		runtime->mcp_job_count++;
		rc = nb_thread_create(&job->thread, job_run, job, NULL);
		if (rc == 0) {
			job->started = 1;
		} else {
			job->active = 0;
			job->state = RUNTIME_MCP_FAILED;
			job->error_code = rc;
			runtime_mcp_emit(runtime->options.event_cb,
					 runtime->options.event_user_data, "mcp.failed", "failed",
					 "MCP worker could not start", &client->config, -1, -1, -1,
					 rc);
		}
	}
	return 0;
}

void runtime_mcp_background_stop(struct runtime *runtime)
{
	for (int i = 0; i < runtime->mcp_job_count; i++) {
		struct runtime_mcp_job *job = &runtime->mcp_jobs[i];

		nb_mutex_lock(job->lock);
		nb_cancel_request(&job->cancel);
		nb_mutex_unlock(job->lock);
	}
	for (int i = 0; i < runtime->mcp_job_count; i++) {
		struct runtime_mcp_job *job = &runtime->mcp_jobs[i];

		if (job->started)
			nb_thread_join_take(&job->thread, NULL);
		(nb_mutex_free(job->lock), job->lock = NULL);
	}
	runtime->mcp_job_count = 0;
}
