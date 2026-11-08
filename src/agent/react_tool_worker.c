#include "react_tool_worker.h"
#include "util/cleanup.h"

#include "react_internal.h"
#include "tool_execution.h"
#include "util/child_io.h"
#include "util/error.h"
#include "util/log.h"
#include "util/signals.h"
#include "util/utf8.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** \brief Enter a request callback only while its borrowed context is open. */
static int tool_callback_enter(struct async_tool_call *call)
{
	int allowed;
	pthread_mutex_lock(&call->mutex);
	allowed = !call->callbacks_closed;
	if (allowed) {
		call->callbacks_active++;
	}
	pthread_mutex_unlock(&call->mutex);
	return allowed;
}

/** \brief Publish callback completion for cancellation and owner shutdown. */
static void tool_callback_leave(struct async_tool_call *call)
{
	pthread_mutex_lock(&call->mutex);
	call->callbacks_active--;
	pthread_cond_broadcast(&call->cond);
	pthread_mutex_unlock(&call->mutex);
}

/** \brief Forward an event without allowing late request callbacks. */
static int tool_callback_event(const struct morph_event *event, void *opaque)
{
	struct async_tool_call *call = opaque;
	int rc = -ECANCELED;
	if (tool_callback_enter(call)) {
		rc = call->runtime.event_cb
			     ? call->runtime.event_cb(event, call->runtime.event_user_data)
			     : 0;
		tool_callback_leave(call);
	}
	return rc;
}

/** \brief Gate borrowed frontend question callbacks. */
static int tool_callback_ask(const char *question, const char *const *choices, int count,
			     const char *mode, int min, int max,
			     const struct ask_user_options *options, char ***answers,
			     int *answer_count, void *opaque)
{
	struct async_tool_call *call = opaque;
	int rc = -ECANCELED;
	*answers = NULL;
	*answer_count = 0;
	if (tool_callback_enter(call)) {
		rc = call->runtime.ask_user_fn
			     ? call->runtime.ask_user_fn(question, choices, count, mode, min, max,
							 options, answers, answer_count,
							 call->runtime.ask_user_user_data)
			     : -ENOSYS;
		tool_callback_leave(call);
	}
	return rc;
}

/** \brief Close callbacks and transfer a joinable worker to its existing owner. */
void react_tool_worker_retire(struct react_context *ctx, struct async_tool_call *call,
			      pthread_t thread)
{
	pthread_mutex_lock(&call->mutex);
	call->cancelled = 1;
	morph_cancel_token_cancel(&call->cancel_token);
	call->callbacks_closed = 1;
	call->start_released = 1;
	pthread_cond_broadcast(&call->cond);
	while (call->callbacks_active != 0) {
		pthread_cond_wait(&call->cond, &call->mutex);
	}
	pthread_mutex_unlock(&call->mutex);
	call->thread = thread;
	call->retired_next = ctx->retired_tool_calls;
	ctx->retired_tool_calls = call;
}

struct async_tool_call *react_tool_worker_create(struct tool_registry *tools,
						 struct react_context *react, const char *tool_name,
						 const char *tool_args, const char *tool_call_id,
						 const char *provider_tool_call_id,
						 react_output_cb output_cb, void *output_user_data)
{
	struct async_tool_call *call = calloc(1, sizeof(*call));
	if (!call)
		return NULL;
	int init_rc = pthread_mutex_init(&call->mutex, NULL);
	if (init_rc != 0) {
		free(call);
		return NULL;
	}
	pthread_condattr_t attributes;
	init_rc = pthread_condattr_init(&attributes);
	if (init_rc != 0) {
		pthread_mutex_destroy(&call->mutex);
		free(call);
		return NULL;
	}
#ifdef __linux__
	init_rc = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
#endif
	if (init_rc == 0) {
		init_rc = pthread_cond_init(&call->cond, &attributes);
	}
	pthread_condattr_destroy(&attributes);
	if (init_rc != 0) {
		pthread_mutex_destroy(&call->mutex);
		free(call);
		return NULL;
	}
	call->tools = tools;
	call->react = react;
	call->started_ms = morph_io_now_ms();
	call->runtime = react->tool_runtime;
	call->runtime.event_cb = react->event_cb;
	call->runtime.event_user_data = react->event_user_data;
	call->runtime.ask_user_fn = react->ask_user_fn;
	call->runtime.ask_user_user_data = react->ask_user_data;
	call->runtime.turn_id = react->turn_id;
	call->runtime.sub_agent_depth = react->sub_agent_depth;
	call->tool_name = strdup(tool_name ? tool_name : "");
	call->tool_args = strdup(tool_args ? tool_args : "{}");
	call->tool_call_id = strdup(tool_call_id ? tool_call_id : "");
	call->provider_tool_call_id = strdup(provider_tool_call_id ? provider_tool_call_id : "");
	call->output_cb = output_cb;
	call->output_user_data = output_user_data;
	morph_cancel_token_reset(&call->cancel_token);
	if (!call->tool_name || !call->tool_args || !call->tool_call_id ||
	    !call->provider_tool_call_id) {
		free(call->tool_name);
		free(call->tool_args);
		free(call->tool_call_id);
		free(call->provider_tool_call_id);
		pthread_cond_destroy(&call->cond);
		pthread_mutex_destroy(&call->mutex);
		free(call);
		return NULL;
	}
	return call;
}

void react_tool_worker_destroy(struct async_tool_call *call)
{
	if (!call)
		return;
	pthread_cond_destroy(&call->cond);
	pthread_mutex_destroy(&call->mutex);
	free(call->tool_name);
	free(call->tool_args);
	free(call->tool_call_id);
	free(call->provider_tool_call_id);
	free(call->result);
	cJSON_Delete(call->data);
	cJSON_Delete(call->ui);
	cJSON_Delete(call->meta);
	free(call);
}

/** \brief Join through the borrowed instance adapter or the native pthread API. */
static int react_join_worker(struct react_context *ctx, pthread_t thread)
{
	return ctx->thread_ops != NULL && ctx->thread_ops->join != NULL
		       ? ctx->thread_ops->join(thread, ctx->thread_ops->user_data)
		       : pthread_join(thread, NULL);
}

/**
 * \brief Reclaim cancelled workers before reusing or destroying their dependencies.
 * \param[in,out] ctx: Borrowed owner-thread context, nullable for an empty operation.
 * \note May block for non-cooperative tools. All other borrowed dependencies must
 *       remain alive until return. Request callbacks were closed during retirement.
 */
void react_drain_tools(struct react_context *ctx)
{
	while (ctx != NULL && ctx->retired_tool_calls != NULL) {
		struct async_tool_call *call = ctx->retired_tool_calls;
		int rc = react_join_worker(ctx, call->thread);
		if (rc != 0) {
			log_err(ctx->logger, "tool worker join failed: %s", morph_strerror(-rc));
			while (!__atomic_load_n(&call->exited, __ATOMIC_ACQUIRE)) {
				struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
				nanosleep(&pause, NULL);
			}
			(void)pthread_detach(call->thread);
		}
		ctx->retired_tool_calls = call->retired_next;
		react_tool_worker_destroy(call);
	}
}

/* Cancellation stops waiting and closes callbacks. The owner joins retired calls
 * before resetting the context or freeing borrowed dependencies. */
int react_tool_worker_join(struct react_context *ctx, pthread_t thread,
			   volatile sig_atomic_t *cancelled, struct async_tool_call *call,
			   int timeout_seconds)
{
	struct timespec ts;
	int64_t deadline;

	deadline = timeout_seconds > 0 ? call->started_ms + (int64_t)timeout_seconds * 1000 : 0;

	pthread_mutex_lock(&call->mutex);
	while (!call->completed && !(morph_signal_flag_get(cancelled))) {
		if (deadline > 0 && morph_io_now_ms() >= deadline)
			break;
#ifdef __linux__
		clock_gettime(CLOCK_MONOTONIC, &ts);
#else
		clock_gettime(CLOCK_REALTIME, &ts);
#endif
		ts.tv_nsec += 100 * 1000 * 1000;
		if (ts.tv_nsec >= 1000 * 1000 * 1000) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000 * 1000 * 1000;
		}
#ifdef __APPLE__
		ts.tv_sec = 0;
		ts.tv_nsec = 100 * 1000 * 1000;
		pthread_cond_timedwait_relative_np(&call->cond, &call->mutex, &ts);
#else
		pthread_cond_timedwait(&call->cond, &call->mutex, &ts);
#endif
	}
	if (call->completed || !morph_signal_flag_get(cancelled)) {
		if (!call->completed && deadline > 0 && morph_io_now_ms() >= deadline) {
			pthread_mutex_unlock(&call->mutex);
			react_tool_worker_retire(ctx, call, thread);
			return -ETIMEDOUT;
		}
		pthread_mutex_unlock(&call->mutex);
		int join_rc = react_join_worker(ctx, thread);
		if (join_rc != 0) {
			react_tool_worker_retire(ctx, call, thread);
			return -join_rc;
		}
		return 0;
	}

	pthread_mutex_unlock(&call->mutex);
	react_tool_worker_retire(ctx, call, thread);
	return -ECANCELED;
}

static char *react_format_tool_error(const struct tool_result *result, int rc)
{
	const cJSON *error;
	const cJSON *message;
	const char *raw;
	const char *detail = NULL;
	morph_buf_t text MORPH_CLEANUP(morph_buf_cleanup) = {0};

	if (result && cJSON_IsObject(result->envelope)) {
		error = cJSON_GetObjectItemCaseSensitive(result->envelope, "error");
		message = cJSON_IsObject(error) ? cJSON_GetObjectItemCaseSensitive(error, "message")
						: NULL;
		if (cJSON_IsString(message) && message->valuestring[0])
			detail = message->valuestring;
	}
	raw = result && result->text.data ? result->text.data : "unknown error";
	if (morph_buf_init(&text, strlen(detail ? detail : raw) + 64) != 0)
		return NULL;
	if (detail)
		(void)morph_buf_printf(&text, "tool error: %s", detail);
	else if (strncmp(raw, "tool error: ", strlen("tool error: ")) == 0)
		(void)morph_buf_puts(&text, raw);
	else
		(void)morph_buf_printf(&text, "tool error: %s (%s)", raw, morph_strerror(rc));
	return morph_buf_detach(&text);
}

static void *react_tool_worker_run(void *arg)
{
	struct async_tool_call *call = (struct async_tool_call *)arg;
	if (!call)
		return NULL;

	pthread_mutex_lock(&call->mutex);
	while (!call->start_released && !call->cancelled)
		pthread_cond_wait(&call->cond, &call->mutex);
	pthread_mutex_unlock(&call->mutex);

	int notify_done = 0;

	if (tool_is_disabled(call->tools, call->tool_name)) {
		char disabled_msg[256];
		snprintf(disabled_msg, sizeof(disabled_msg),
			 "tool error: '%s' is disabled in configuration", call->tool_name);

		pthread_mutex_lock(&call->mutex);
		call->result = strdup(disabled_msg);
		call->rc = -EPERM;
		call->completed = 1;
		pthread_cond_broadcast(&call->cond);
		pthread_mutex_unlock(&call->mutex);
		notify_done = 1;
	} else {
		struct tool_result res;
		struct tool_runtime_context runtime;
		int rc;

		tool_result_init(&res);
		runtime = call->runtime;
		runtime.http.logger = call->react->logger;
		runtime.http.cancel_flag = NULL;
		runtime.http.cancel_token = &call->cancel_token;
		runtime.http.interrupt_check = NULL;
		runtime.http.interrupt_user_data = NULL;
		runtime.event_cb = tool_callback_event;
		runtime.event_user_data = call;
		runtime.ask_user_fn = tool_callback_ask;
		runtime.ask_user_user_data = call;
		runtime.tool_call_id = call->tool_call_id;
		res.runtime = &runtime;
		rc = morph_cancel_token_is_cancelled(&call->cancel_token)
			     ? -ECANCELED
			     : tool_exec(call->tools, call->tool_name, call->tool_args, &res);

		pthread_mutex_lock(&call->mutex);
		int was_cancelled = call->cancelled;
		if (was_cancelled) {
			call->completed = 1;
			pthread_cond_broadcast(&call->cond);
		} else if (rc < 0) {
			call->result = react_format_tool_error(&res, rc);
			call->meta = res.meta ? cJSON_Duplicate(res.meta, 1) : NULL;
			call->rc = rc;
			call->completed = 1;
			pthread_cond_broadcast(&call->cond);
		} else {
			const char *raw = res.text.data ? res.text.data : "(no output)";
				call->result = strdup(raw);
			call->data = res.data ? cJSON_Duplicate(res.data, 1) : NULL;
			call->ui = res.ui ? cJSON_Duplicate(res.ui, 1) : NULL;
			call->meta = res.meta ? cJSON_Duplicate(res.meta, 1) : NULL;
			call->artifacts = res.artifacts;
			call->rc = call->result == NULL ||
						   (res.data != NULL && call->data == NULL) ||
						   (res.ui != NULL && call->ui == NULL) ||
						   (res.meta != NULL && call->meta == NULL)
					   ? -ENOMEM
					   : 0;
			call->completed = 1;
			pthread_cond_broadcast(&call->cond);
		}
		pthread_mutex_unlock(&call->mutex);

		tool_result_cleanup(&res);
		notify_done = !was_cancelled;
	}

	if (notify_done && tool_callback_enter(call)) {
		enum react_output_status status =
			call->rc < 0 ? REACT_OUTPUT_FAILED : REACT_OUTPUT_COMPLETED;
		react_output_emit(call->output_cb, call->output_user_data,
				  &(struct react_output_event){
					  .type = REACT_STEP_ACTION,
					  .status = status,
					  .tool_name = call->tool_name,
					  .tool_args = call->tool_args,
					  .tool_call_id = call->tool_call_id,
					  .error_code = call->rc,
				  });
		tool_callback_leave(call);
	}

	__atomic_store_n(&call->exited, 1, __ATOMIC_RELEASE);
	return NULL;
}

void react_tool_worker_collect(struct async_tool_call *call, int *rc, char **result,
			       struct tool_artifact_list *artifacts, cJSON **data, cJSON **ui,
			       cJSON **meta)
{
	pthread_mutex_lock(&call->mutex);
	*rc = call->rc;
	*result = call->result;
	if (artifacts)
		*artifacts = call->artifacts;
	if (data) {
		*data = call->data;
		call->data = NULL;
	}
	if (ui) {
		*ui = call->ui;
		call->ui = NULL;
	}
	if (meta) {
		*meta = call->meta;
		call->meta = NULL;
	}
	call->result = NULL;
	memset(&call->artifacts, 0, sizeof(call->artifacts));
	pthread_mutex_unlock(&call->mutex);
}

int react_tool_worker_start(struct async_tool_call *call, pthread_t *thread, int require_user)
{
	call->runtime.permission_require_user = require_user;
	return -pthread_create(thread, NULL, react_tool_worker_run, call);
}

void react_tool_worker_release(struct async_tool_call *call)
{
	pthread_mutex_lock(&call->mutex);
	call->start_released = 1;
	pthread_cond_broadcast(&call->cond);
	pthread_mutex_unlock(&call->mutex);
}

void react_tool_worker_cancel(struct async_tool_call *call)
{
	pthread_mutex_lock(&call->mutex);
	call->cancelled = 1;
	morph_cancel_token_cancel(&call->cancel_token);
	pthread_mutex_unlock(&call->mutex);
}
