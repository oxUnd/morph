#include "runtime/runtime_internal.h"
#include "runtime/mcp.h"
#include "util/error.h"
#include "util/log.h"

#include <errno.h>
#include <string.h>
#include <time.h>

int64_t runtime_mcp_now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
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

	pthread_mutex_lock(&job->lock);
	cancelled = morph_cancel_token_is_cancelled(&job->cancel);
	state = job->state;
	pthread_mutex_unlock(&job->lock);
	if (cancelled)
		return 1;
	return state != RUNTIME_MCP_REGISTERING &&
		job->client->config.connect_timeout > 0 &&
		runtime_mcp_now_ms() - job->started_ms >=
		(int64_t)job->client->config.connect_timeout * 1000;
}

static void job_state(struct runtime_mcp_job *job, enum runtime_mcp_state state)
{
	pthread_mutex_lock(&job->lock);
	job->state = state;
	pthread_mutex_unlock(&job->lock);
}

static int job_event(const struct morph_event *event, void *opaque);

/* Transfer ownership only between turns. Discovery never touches live tools. */
static int job_publish(struct runtime_mcp_job *job, struct tool_registry *pending)
{
	struct runtime_context *ctx = &job->runtime->context;
	struct timespec pause = { .tv_nsec = 10000000 };
	int rc;
	int added = 0;

	job_state(job, RUNTIME_MCP_REGISTERING);
	runtime_mcp_emit(job_event, job, "mcp.registering", "pending",
		"Waiting to register MCP capabilities between turns",
		&job->client->config, -1, -1, -1, 0);
	for (;;) {
		if (job_interrupted(job))
			MORPH_RETURN_LOG(job->runtime->options.logger, -ECANCELED);
		rc = pthread_mutex_trylock(&ctx->execution_lock);
		if (rc == 0)
			break;
		if (rc != EBUSY)
			MORPH_RETURN_LOG(job->runtime->options.logger, -rc);
		nanosleep(&pause, NULL);
	}
	pthread_mutex_lock(&ctx->tool_registry_lock);
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

		if (tool_lookup(&ctx->tools, spec.name)) {
			rc = -EEXIST;
			break;
		}
		rc = tool_register(&ctx->tools, &spec);
		if (rc != 0)
			break;
		entry->user_data = NULL;
		entry->user_data_destroy = NULL;
		added++;
	}
	if (rc != 0) {
		for (int i = 0; i < added; i++)
			(void)tool_unregister(&ctx->tools, pending->entries[i].desc.name);
	}
	pthread_mutex_unlock(&ctx->tool_registry_lock);
	pthread_mutex_unlock(&ctx->execution_lock);
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
	pthread_mutex_lock(&job->lock);
	if (rc != 0 && morph_cancel_token_is_cancelled(&job->cancel))
		rc = -ECANCELED;
	else if (rc != 0 && job->client->config.connect_timeout > 0 &&
		 runtime_mcp_now_ms() - job->started_ms >=
		 (int64_t)job->client->config.connect_timeout * 1000)
		rc = -ETIMEDOUT;
	job->client->discovery_error = rc;
	job->client->capabilities_registered = rc == 0;
	job->error_code = rc;
	job->elapsed_ms = runtime_mcp_now_ms() - job->started_ms;
	job->state = rc == 0 ? RUNTIME_MCP_READY : RUNTIME_MCP_FAILED;
	job->active = 0;
	pthread_mutex_unlock(&job->lock);
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
		rc = pthread_mutex_init(&job->lock, NULL);
		if (rc != 0) {
			log_warn(job->runtime->options.logger, "mcp: worker lock failed: %s",
				 morph_strerror(-rc));
			continue;
		}
		job->runtime = runtime;
		job->client = client;
		job->state = RUNTIME_MCP_CONNECTING;
		job->active = 1;
		job->started_ms = runtime_mcp_now_ms();
		runtime->mcp_job_count++;
		rc = pthread_create(&job->thread, NULL, job_run, job);
		if (rc == 0) {
			job->started = 1;
		} else {
			job->active = 0;
			job->state = RUNTIME_MCP_FAILED;
			job->error_code = -rc;
			runtime_mcp_emit(runtime->options.event_cb,
				runtime->options.event_user_data, "mcp.failed", "failed",
				"MCP worker could not start", &client->config, -1, -1, -1, -rc);
		}
	}
	return 0;
}

void runtime_mcp_background_stop(struct runtime *runtime)
{
	for (int i = 0; i < runtime->mcp_job_count; i++) {
		struct runtime_mcp_job *job = &runtime->mcp_jobs[i];

		pthread_mutex_lock(&job->lock);
		morph_cancel_token_cancel(&job->cancel);
		pthread_mutex_unlock(&job->lock);
	}
	for (int i = 0; i < runtime->mcp_job_count; i++) {
		struct runtime_mcp_job *job = &runtime->mcp_jobs[i];

		if (job->started)
			pthread_join(job->thread, NULL);
		pthread_mutex_destroy(&job->lock);
	}
	runtime->mcp_job_count = 0;
}
