#include "react_tool_worker.h"
#include "nb/time.h"
#include "nb/cleanup.h"

#include "react_internal.h"
#include "tool_execution.h"
#include "errors.h"
#include "nb/log.h"
#include "nb/signal.h"
#include "nb/utf8.h"

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
	nb_mutex_lock(call->mutex);
	allowed = !call->callbacks_closed;
	if (allowed) {
		call->callbacks_active++;
	}
	nb_mutex_unlock(call->mutex);
	return allowed;
}

/** \brief Publish callback completion for cancellation and owner shutdown. */
static void tool_callback_leave(struct async_tool_call *call)
{
	nb_mutex_lock(call->mutex);
	call->callbacks_active--;
	nb_cond_broadcast(call->cond);
	nb_mutex_unlock(call->mutex);
}

/** \brief Forward an event without allowing late request callbacks. */
static int tool_callback_event(const struct morph_event *event, void *opaque)
{
	struct async_tool_call *call = opaque;
	int rc = NB_ERR_CANCELLED;
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
	int rc = NB_ERR_CANCELLED;
	*answers = NULL;
	*answer_count = 0;
	if (tool_callback_enter(call)) {
		rc = call->runtime.ask_user_fn
			     ? call->runtime.ask_user_fn(question, choices, count, mode, min, max,
							 options, answers, answer_count,
							 call->runtime.ask_user_user_data)
			     : NB_ERR_UNSUPPORTED;
		tool_callback_leave(call);
	}
	return rc;
}

static int tool_callback_input_pending(void *opaque)
{
	struct async_tool_call *call = opaque;
	int pending = 0;
	if (tool_callback_enter(call)) {
		pending = call->react->prompt_pending_fn
			? call->react->prompt_pending_fn(call->react->action_drain_user_data) : 0;
		tool_callback_leave(call);
	}
	return pending;
}

/** \brief Close callbacks and transfer a joinable worker to its existing owner. */
void react_tool_worker_retire(struct react_context *ctx, struct async_tool_call *call,
			      nb_thread *thread)
{
	nb_mutex_lock(call->mutex);
	call->cancelled = 1;
	nb_cancel_request(&call->cancel_token);
	call->callbacks_closed = 1;
	call->start_released = 1;
	nb_cond_broadcast(call->cond);
	while (call->callbacks_active != 0) {
		nb_cond_wait(call->cond, call->mutex);
	}
	nb_mutex_unlock(call->mutex);
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
	int init_rc = ((call->mutex = nb_mutex_new(0)) ? NB_OK : NB_ERR_NOMEM);
	if (init_rc != 0) {
		free(call);
		return NULL;
	}
	call->cond = nb_cond_new();
	init_rc = call->cond ? NB_OK : NB_ERR_NOMEM;
	if (init_rc != 0) {
		(nb_mutex_free(call->mutex), call->mutex = NULL);
		free(call);
		return NULL;
	}
	call->tools = tools;
	call->react = react;
	call->started_ms = nb_time_monotonic_ms();
	call->runtime = react->tool_runtime;
	call->runtime.event_cb = react->event_cb;
	call->runtime.event_user_data = react->event_user_data;
	call->runtime.ask_user_fn = react->ask_user_fn;
	call->runtime.ask_user_user_data = react->ask_user_data;
	call->runtime.turn_id = react->turn_id;
	call->runtime.agents = react->agents;
	call->runtime.agent_depth = react->agent_depth;
	call->runtime.agent_id = react->agent_id[0] ? react->agent_id : "/root";
	call->runtime.workdir = react->workdir;
	call->tool_name = strdup(tool_name ? tool_name : "");
	call->tool_args = strdup(tool_args ? tool_args : "{}");
	call->tool_call_id = strdup(tool_call_id ? tool_call_id : "");
	call->provider_tool_call_id = strdup(provider_tool_call_id ? provider_tool_call_id : "");
	call->output_cb = output_cb;
	call->output_user_data = output_user_data;
	nb_cancel_reset(&call->cancel_token);
	if (!call->tool_name || !call->tool_args || !call->tool_call_id ||
	    !call->provider_tool_call_id) {
		free(call->tool_name);
		free(call->tool_args);
		free(call->tool_call_id);
		free(call->provider_tool_call_id);
		(nb_cond_free(call->cond), call->cond = NULL);
		(nb_mutex_free(call->mutex), call->mutex = NULL);
		free(call);
		return NULL;
	}
	return call;
}

void react_tool_worker_destroy(struct async_tool_call *call)
{
	if (!call)
		return;
	(nb_cond_free(call->cond), call->cond = NULL);
	(nb_mutex_free(call->mutex), call->mutex = NULL);
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

/** \brief Join through the borrowed instance adapter or the native nbase API. */
static int react_join_worker(struct react_context *ctx, nb_thread *thread)
{
	return ctx->thread_ops != NULL && ctx->thread_ops->join != NULL
		       ? ctx->thread_ops->join(thread, ctx->thread_ops->user_data)
		       : nb_thread_join_take(&thread, NULL);
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
			nb_log_write(ctx->logger, NB_LOG_ERROR, "tool worker join failed: %s",
				     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
			while (!__atomic_load_n(&call->exited, __ATOMIC_ACQUIRE)) {
				struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
				nanosleep(&pause, NULL);
			}
			(void)nb_thread_detach_take(&call->thread);
		}
		ctx->retired_tool_calls = call->retired_next;
		react_tool_worker_destroy(call);
	}
}

/* Cancellation stops waiting and closes callbacks. The owner joins retired calls
 * before resetting the context or freeing borrowed dependencies. */
int react_tool_worker_join(struct react_context *ctx, nb_thread *thread, nb_atomic_int *cancelled,
			   struct async_tool_call *call, int timeout_seconds)
{
	int64_t deadline;
	int interrupted = 0;

	deadline = timeout_seconds > 0 ? call->started_ms + (int64_t)timeout_seconds * 1000 : 0;

	nb_mutex_lock(call->mutex);
	while (!call->completed && !(nb_atomic_int_load(cancelled))) {
		if (ctx->prompt_pending_fn &&
		    ctx->prompt_pending_fn(ctx->action_drain_user_data)) {
			interrupted = 1;
			break;
		}
		if (deadline > 0 && nb_time_monotonic_ms() >= deadline)
			break;
		(void)nb_cond_timedwait(call->cond, call->mutex, INT64_C(100000000));
	}
	if (call->completed || (!nb_atomic_int_load(cancelled) && !interrupted)) {
		if (!call->completed && deadline > 0 && nb_time_monotonic_ms() >= deadline) {
			nb_mutex_unlock(call->mutex);
			react_tool_worker_retire(ctx, call, thread);
			return NB_ERR_TIMEOUT;
		}
		nb_mutex_unlock(call->mutex);
		int join_rc = react_join_worker(ctx, thread);
		if (join_rc != 0) {
			react_tool_worker_retire(ctx, call, thread);
			return join_rc;
		}
		return 0;
	}

	nb_mutex_unlock(call->mutex);
	react_tool_worker_retire(ctx, call, thread);
	return nb_atomic_int_load(cancelled) ? NB_ERR_CANCELLED : NB_ERR_INTERRUPTED;
}

static char *react_format_tool_error(const struct tool_result *result, int rc)
{
	const cJSON *error;
	const cJSON *message;
	const char *raw;
	const char *detail = NULL;
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};

	if (result && cJSON_IsObject(result->envelope)) {
		error = cJSON_GetObjectItemCaseSensitive(result->envelope, "error");
		message = cJSON_IsObject(error) ? cJSON_GetObjectItemCaseSensitive(error, "message")
						: NULL;
		if (cJSON_IsString(message) && message->valuestring[0])
			detail = message->valuestring;
	}
	raw = result && result->text.ptr ? result->text.ptr : "unknown error";
	if ((nb_buf_init(&text),
	     nb_buf_reserve(&text, strlen(detail ? detail : raw) + 64) ? 0 : NB_ERR_NOMEM) != 0)
		return NULL;
	if (detail)
		(void)nb_buf_printf(&text, "tool error: %s", detail);
	else if (strncmp(raw, "tool error: ", strlen("tool error: ")) == 0)
		(void)nb_buf_append_cstr(&text, raw);
	else
		(void)nb_buf_printf(&text, "tool error: %s (%s)", raw,
				    nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	return (nb_buf_ok(&text) ? strdup(nb_buf_cstr(&text)) : NULL);
}

static void *react_tool_worker_run(void *arg)
{
	struct async_tool_call *call = (struct async_tool_call *)arg;
	if (!call)
		return NULL;

	nb_mutex_lock(call->mutex);
	while (!call->start_released && !call->cancelled)
		nb_cond_wait(call->cond, call->mutex);
	nb_mutex_unlock(call->mutex);

	int notify_done = 0;

	if (tool_is_disabled(call->tools, call->tool_name)) {
		char disabled_msg[256];
		snprintf(disabled_msg, sizeof(disabled_msg),
			 "tool error: '%s' is disabled in configuration", call->tool_name);

		nb_mutex_lock(call->mutex);
		call->result = strdup(disabled_msg);
		call->rc = NB_ERR_NOT_PERMITTED;
		call->completed = 1;
		nb_cond_broadcast(call->cond);
		nb_mutex_unlock(call->mutex);
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
		runtime.input_pending_fn = tool_callback_input_pending;
		runtime.input_pending_user_data = call;
		runtime.event_cb = tool_callback_event;
		runtime.event_user_data = call;
		runtime.ask_user_fn = tool_callback_ask;
		runtime.ask_user_user_data = call;
		runtime.tool_call_id = call->tool_call_id;
		res.runtime = &runtime;
		rc = nb_cancel_requested(&call->cancel_token)
			     ? NB_ERR_CANCELLED
			     : tool_exec(call->tools, call->tool_name, call->tool_args, &res);

		nb_mutex_lock(call->mutex);
		int was_cancelled = call->cancelled;
		if (was_cancelled) {
			call->completed = 1;
			nb_cond_broadcast(call->cond);
		} else if (rc < 0) {
			call->result = react_format_tool_error(&res, rc);
			call->meta = res.meta ? cJSON_Duplicate(res.meta, 1) : NULL;
			call->rc = rc;
			call->completed = 1;
			nb_cond_broadcast(call->cond);
		} else {
			const char *raw = res.text.ptr ? res.text.ptr : "(no output)";
			call->result = strdup(raw);
			call->data = res.data ? cJSON_Duplicate(res.data, 1) : NULL;
			call->ui = res.ui ? cJSON_Duplicate(res.ui, 1) : NULL;
			call->meta = res.meta ? cJSON_Duplicate(res.meta, 1) : NULL;
			call->artifacts = res.artifacts;
			call->rc = call->result == NULL ||
						   (res.data != NULL && call->data == NULL) ||
						   (res.ui != NULL && call->ui == NULL) ||
						   (res.meta != NULL && call->meta == NULL)
					   ? NB_ERR_NOMEM
					   : 0;
			call->completed = 1;
			nb_cond_broadcast(call->cond);
		}
		nb_mutex_unlock(call->mutex);

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
	nb_mutex_lock(call->mutex);
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
	nb_mutex_unlock(call->mutex);
}

int react_tool_worker_start(struct async_tool_call *call, nb_thread **thread, int require_user)
{
	call->runtime.permission_require_user = require_user;
	return nb_thread_create(thread, react_tool_worker_run, call, NULL);
}

void react_tool_worker_release(struct async_tool_call *call)
{
	nb_mutex_lock(call->mutex);
	call->start_released = 1;
	nb_cond_broadcast(call->cond);
	nb_mutex_unlock(call->mutex);
}

void react_tool_worker_cancel(struct async_tool_call *call)
{
	nb_mutex_lock(call->mutex);
	call->cancelled = 1;
	nb_cancel_request(&call->cancel_token);
	nb_mutex_unlock(call->mutex);
}
