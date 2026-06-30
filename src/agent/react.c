#include "react.h"
#include "guardrail.h"
#include "tokenizer.h"
#include "compress.h"
#include "system_prompt.h"
#include "models/llm.h"
#include "http/client.h"
#include "util/log.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/array.h"
#include "util/utf8.h"
#include "util/error.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>

volatile sig_atomic_t react_sigint_flag = 0;

#define REACT_ACTIVE_MAX 16

static struct react_context *react_active_stack[REACT_ACTIVE_MAX];
static int react_active_count_val = 0;
static pthread_mutex_t react_active_mutex = PTHREAD_MUTEX_INITIALIZER;

int react_active_count(void)
{
	int c;

	pthread_mutex_lock(&react_active_mutex);
	c = react_active_count_val;
	pthread_mutex_unlock(&react_active_mutex);
	return c;
}

void react_active_push(struct react_context *ctx)
{
	pthread_mutex_lock(&react_active_mutex);
	if (react_active_count_val < REACT_ACTIVE_MAX)
		react_active_stack[react_active_count_val++] = ctx;
	pthread_mutex_unlock(&react_active_mutex);
}

void react_active_pop(struct react_context *ctx)
{
	pthread_mutex_lock(&react_active_mutex);
	for (int i = react_active_count_val - 1; i >= 0; i--) {
		if (react_active_stack[i] == ctx) {
			react_active_stack[i] =
				react_active_stack[react_active_count_val - 1];
			react_active_count_val--;
			break;
		}
	}
	pthread_mutex_unlock(&react_active_mutex);
}

void react_cancel_active(void)
{
	pthread_mutex_lock(&react_active_mutex);
	for (int i = 0; i < react_active_count_val; i++) {
		react_active_stack[i]->cancelled = 1;
		morph_cancel_token_cancel(&react_active_stack[i]->cancel_token);
	}
	pthread_mutex_unlock(&react_active_mutex);
	react_sigint_flag = 1;
}

/*
 * Check whether a tool requires human-in-the-loop approval.
 *
 * ctx - ReAct context with HITL configuration
 * tool_name - Name of the tool to check
 *
 * Returns 1 if approval is required, 0 if auto-approved or HITL disabled.
 */
int hitl_needs_approval(struct react_context *ctx, const char *tool_name)
{
	if (!ctx || !tool_name)
		return 0;
	struct hitl_config *h = &ctx->hitl;
	if (!h->enabled)
		return 0;
	for (int i = 0; i < h->auto_approved_count; i++) {
		if (strcmp(h->auto_approved[i], tool_name) == 0)
			return 0;
	}
	if (tool_has_flag(ctx->tools, tool_name, TOOL_FLAG_INTERNAL_APPROVAL))
		return 0;
	if (h->tools_count > 0) {
		for (int i = 0; i < h->tools_count; i++) {
			if (strcmp(h->tools[i], tool_name) == 0)
				return 1;
		}
		return 0;
	}
	if (h->auto_approve_readonly && tool_is_readonly(ctx->tools, tool_name))
		return 0;
	return 1;
}

/*
 * Add a tool to the HITL auto-approved list so it bypasses future approval checks.
 *
 * h - HITL configuration to update
 * tool_name - Name of the tool to auto-approve
 */
void hitl_add_auto_approved(struct hitl_config *h, const char *tool_name)
{
	if (!h || !tool_name)
		return;
	if (h->auto_approved_count >= HITL_AUTO_APPROVED_MAX)
		return;
	for (int i = 0; i < h->auto_approved_count; i++) {
		if (strcmp(h->auto_approved[i], tool_name) == 0)
			return;
	}
	strncpy(h->auto_approved[h->auto_approved_count], tool_name,
		HITL_TOOL_NAME_MAX - 1);
	h->auto_approved_count++;
}

/*
 * Register an action drain callback for external action injection.
 *
 * ctx - ReAct context to configure
 * fn - Callback function, or NULL to clear
 * user - Opaque pointer passed to fn
 *
 * Returns 0 on success, -EINVAL if ctx is NULL.
 */
int react_set_action_drain(struct react_context *ctx,
			   react_action_drain_fn fn, void *user)
{
	if (!ctx)
		return -EINVAL;
	ctx->action_drain_fn = fn;
	ctx->action_drain_user_data = user;
	return 0;
}

int react_set_event_callback(struct react_context *ctx,
			     morph_event_cb cb, void *user)
{
	if (!ctx)
		return -EINVAL;
	ctx->event_cb = cb;
	ctx->event_user_data = user;
	return 0;
}

static int react_events_enabled(struct react_context *ctx)
{
	return ctx && ctx->event_cb;
}

static int react_emit_event(struct react_context *ctx,
			    enum morph_event_type type, const char *name,
			    const char *phase, const char *message,
			    cJSON *data)
{
	if (!ctx || !ctx->event_cb)
		return 0;
	return morph_event_emit_simple(ctx->event_cb, ctx->event_user_data,
				       type, name, phase, message, data);
}

static int react_emit_text_event(struct react_context *ctx,
				 enum morph_event_type type,
				 const char *name, const char *phase,
				 const char *message, const char *text)
{
	cJSON *data;
	int rc;

	if (!react_events_enabled(ctx))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "text", text ? text : "");
	rc = react_emit_event(ctx, type, name, phase, message, data);
	cJSON_Delete(data);
	return rc;
}

static void react_set_state(struct react_context *ctx, enum react_state state)
{
	if (ctx)
		ctx->state = state;
}

static void react_set_result(struct react_context *ctx,
			     enum react_outcome outcome,
			     int error_code, const char *reason)
{
	if (!ctx)
		return;
	ctx->outcome = outcome;
	ctx->last_error_code = error_code;
	ctx->state = outcome == REACT_OUTCOME_SUCCESS ?
		REACT_STATE_DONE : REACT_STATE_ABORT;
	if (reason && *reason) {
		snprintf(ctx->outcome_reason, sizeof(ctx->outcome_reason),
			 "%s", reason);
	} else {
		ctx->outcome_reason[0] = '\0';
	}
}

static int react_emit_outcome_event(struct react_context *ctx,
				    const char *name, const char *phase,
				    const char *message, const char *text)
{
	cJSON *data;
	int rc;

	if (!react_events_enabled(ctx))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "text", text ? text : "");
	cJSON_AddStringToObject(data, "outcome",
				react_outcome_name(ctx ? ctx->outcome :
						   REACT_OUTCOME_NONE));
	if (ctx && ctx->outcome_reason[0])
		cJSON_AddStringToObject(data, "reason", ctx->outcome_reason);
	if (ctx && ctx->last_error_code < 0) {
		cJSON_AddNumberToObject(data, "error_code",
					ctx->last_error_code);
		cJSON_AddStringToObject(data, "error",
					morph_strerror(ctx->last_error_code));
	}
	rc = react_emit_event(ctx, MORPH_EVENT_REACT, name, phase, message,
			      data);
	cJSON_Delete(data);
	return rc;
}

static int react_finish(struct react_context *ctx)
{
	const char *event_name = "react.failed";
	const char *phase = "failed";
	const char *message = "turn failed";
	int rc;

	if (!ctx)
		return -EINVAL;
	if (ctx->outcome == REACT_OUTCOME_NONE) {
		if (ctx->state == REACT_STATE_DONE) {
			react_set_result(ctx, REACT_OUTCOME_SUCCESS, 0, NULL);
		} else if (ctx->cancelled) {
			react_set_result(ctx, REACT_OUTCOME_CANCELLED,
					 -ECANCELED, "user_cancelled");
		} else {
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
					 -ECANCELED, "internal_error");
		}
	}

	rc = ctx->last_error_code < 0 ? ctx->last_error_code : 0;
	if (ctx->outcome == REACT_OUTCOME_SUCCESS) {
		react_emit_outcome_event(ctx, "react.turn.end", "end",
					 "turn completed",
					 ctx->final_answer ?
					 ctx->final_answer : "");
		return 0;
	}

	if (rc == 0)
		rc = -ECANCELED;

	if (ctx->outcome == REACT_OUTCOME_CANCELLED) {
		event_name = "react.cancelled";
		phase = "cancelled";
		message = "turn cancelled";
	} else if (ctx->outcome == REACT_OUTCOME_TIMEOUT) {
		event_name = "react.timed_out";
		phase = "timeout";
		message = "turn timed out";
	} else if (ctx->outcome == REACT_OUTCOME_MAX_ITERATIONS) {
		event_name = "react.max_iterations";
		message = "maximum iterations reached";
	}

	react_emit_outcome_event(ctx, event_name, phase, message,
				 ctx->final_answer ? ctx->final_answer : "");
	react_emit_outcome_event(ctx, "react.turn.end", phase, message,
				 ctx->final_answer ? ctx->final_answer : "");
	return rc;
}

static int react_emit_tool_event(struct react_context *ctx,
				 const char *name, const char *phase,
				 const char *message, const char *tool,
				 const char *args_json,
				 const char *tool_call_id,
				 const char *result, int error_code)
{
	cJSON *data;
	cJSON *args = NULL;
	int rc;

	if (!react_events_enabled(ctx))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "tool", tool ? tool : "");
	cJSON_AddStringToObject(data, "tool_call_id",
				tool_call_id ? tool_call_id : "");
	args = args_json && *args_json ? cJSON_Parse(args_json) : NULL;
	if (!args)
		args = cJSON_CreateObject();
	if (!args) {
		cJSON_Delete(data);
		return -ENOMEM;
	}
	cJSON_AddItemToObject(data, "args", args);
	if (result)
		cJSON_AddStringToObject(data, "result", result);
	if (error_code < 0) {
		cJSON_AddNumberToObject(data, "error_code", error_code);
		cJSON_AddStringToObject(data, "error",
					morph_strerror(error_code));
	}
	rc = react_emit_event(ctx, MORPH_EVENT_TOOL, name, phase, message,
			      data);
	cJSON_Delete(data);
	return rc;
}

static int react_tool_call_begin(struct react_context *ctx,
				 const struct tool_call *tc)
{
	if (!tc)
		return 0;
	return react_emit_tool_event(ctx, "tool.call", "begin",
				     "calling tool", tc->name,
				     tc->arguments ? tc->arguments : "{}",
				     tc->id, NULL, 0);
}

static int react_tool_call_running(struct react_context *ctx,
				   const struct tool_call *tc)
{
	if (!tc)
		return 0;
	return react_emit_tool_event(ctx, "tool.running", "begin",
				     "tool running", tc->name,
				     tc->arguments ? tc->arguments : "{}",
				     tc->id, NULL, 0);
}

static int react_tool_call_thread_failed(struct react_context *ctx,
					 const struct tool_call *tc, int rc)
{
	if (!tc)
		return 0;
	return react_emit_tool_event(ctx, "tool.failed", "failed",
				     "tool thread failed", tc->name,
				     tc->arguments ? tc->arguments : "{}",
				     tc->id, NULL, rc);
}

static int react_emit_artifact_event(struct react_context *ctx,
				     const char *kind, const char *path,
				     const char *source)
{
	cJSON *data;
	int rc;

	if (!react_events_enabled(ctx))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "kind", kind ? kind : "");
	cJSON_AddStringToObject(data, "path", path ? path : "");
	if (source)
		cJSON_AddStringToObject(data, "source", source);
	rc = react_emit_event(ctx, MORPH_EVENT_ARTIFACT, "artifact.ready",
			      "ready", "artifact ready", data);
	cJSON_Delete(data);
	return rc;
}

static void react_emit_artifacts_from_json(struct react_context *ctx,
					   cJSON *root, const char *source)
{
	cJSON *item;
	cJSON *results;

	if (!root)
		return;
	item = cJSON_GetObjectItem(root, "output_path");
	if (cJSON_IsString(item) && item->valuestring)
		react_emit_artifact_event(ctx, "image", item->valuestring,
					  source);
	item = cJSON_GetObjectItem(root, "draft_path");
	if (cJSON_IsString(item) && item->valuestring)
		react_emit_artifact_event(ctx, "image", item->valuestring,
					  source);
	results = cJSON_GetObjectItem(root, "results");
	if (!cJSON_IsArray(results))
		return;
	cJSON_ArrayForEach(item, results) {
		cJSON *path = cJSON_GetObjectItem(item, "output_path");
		if (cJSON_IsString(path) && path->valuestring)
			react_emit_artifact_event(ctx, "image",
						  path->valuestring, source);
	}
}

static void react_emit_artifacts_from_text(struct react_context *ctx,
					   const char *text,
					   const char *source)
{
	cJSON *root;
	const char *prefix;
	const char *path;
	const char *end;
	char *copy;
	size_t len;

	if (!ctx || !text || !*text)
		return;
	root = cJSON_Parse(text);
	if (root) {
		react_emit_artifacts_from_json(ctx, root, source);
		cJSON_Delete(root);
		return;
	}

	prefix = "image generated: ";
	if (strncmp(text, prefix, strlen(prefix)) == 0) {
		path = text + strlen(prefix);
		end = strchr(path, ' ');
		len = end ? (size_t)(end - path) : strlen(path);
		copy = malloc(len + 1);
		if (!copy)
			return;
		memcpy(copy, path, len);
		copy[len] = '\0';
		react_emit_artifact_event(ctx, "image", copy, source);
		free(copy);
		return;
	}

	prefix = "video generated: ";
	if (strncmp(text, prefix, strlen(prefix)) == 0) {
		path = text + strlen(prefix);
		end = strchr(path, ' ');
		len = end ? (size_t)(end - path) : strlen(path);
		copy = malloc(len + 1);
		if (!copy)
			return;
		memcpy(copy, path, len);
		copy[len] = '\0';
		react_emit_artifact_event(ctx, "video", copy, source);
		free(copy);
	}
}

static int react_emit_hitl_event(struct react_context *ctx,
				 const char *name, const char *phase,
				 const char *message, const char *tool,
				 const char *args_json,
				 const char *tool_call_id,
				 const char *verdict)
{
	cJSON *data;
	cJSON *args = NULL;
	int rc;

	if (!react_events_enabled(ctx))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "tool", tool ? tool : "");
	cJSON_AddStringToObject(data, "tool_call_id",
				tool_call_id ? tool_call_id : "");
	args = args_json && *args_json ? cJSON_Parse(args_json) : NULL;
	if (!args)
		args = cJSON_CreateObject();
	if (!args) {
		cJSON_Delete(data);
		return -ENOMEM;
	}
	cJSON_AddItemToObject(data, "args", args);
	if (verdict)
		cJSON_AddStringToObject(data, "verdict", verdict);
	rc = react_emit_event(ctx, MORPH_EVENT_HITL, name, phase, message,
			      data);
	cJSON_Delete(data);
	return rc;
}

/*
 * Per-tool async execution state.
 *
 * Lifetime ownership:
 * - Created by async_tool_call_create() before pthread_create().
 * - Normally destroyed by async_tool_call_destroy() in react_run after join.
 * - On cancellation, ownership is transferred to the worker thread by
 *   setting `detached = 1` under the mutex and calling pthread_detach();
 *   the worker thread then frees the struct itself at exit.
 *
 * tool_name / tool_args / tool_call_id are owned copies so they survive
 * even after the originating chat_response or arena is reset.
 */
struct async_tool_call {
	struct tool_registry *tools;
	struct react_context *react;
	char *tool_name;
	char *tool_args;
	char *tool_call_id;
	char *result;
	int rc;
	react_output_cb output_cb;
	void *output_user_data;
	volatile sig_atomic_t completed;
	volatile sig_atomic_t cancelled;
	volatile sig_atomic_t detached;
	struct morph_cancel_token cancel_token;
	void *usage_user_data;
	pthread_mutex_t mutex;
};

struct react_tool_slot {
	pthread_t thread;
	struct async_tool_call *call;
	int hitl_denied;
	int thread_started;
};

static struct async_tool_call *
async_tool_call_create(struct tool_registry *tools,
		       struct react_context *react,
		       const char *tool_name,
		       const char *tool_args,
		       const char *tool_call_id,
		       react_output_cb output_cb,
		       void *output_user_data)
{
	struct async_tool_call *call = calloc(1, sizeof(*call));
	if (!call)
		return NULL;
	pthread_mutex_init(&call->mutex, NULL);
	call->tools = tools;
	call->react = react;
	call->tool_name = strdup(tool_name ? tool_name : "");
	call->tool_args = strdup(tool_args ? tool_args : "{}");
	call->tool_call_id = strdup(tool_call_id ? tool_call_id : "");
	call->output_cb = output_cb;
	call->output_user_data = output_user_data;
	call->usage_user_data = model_get_usage_user_data();
	morph_cancel_token_reset(&call->cancel_token);
	if (!call->tool_name || !call->tool_args || !call->tool_call_id) {
		free(call->tool_name);
		free(call->tool_args);
		free(call->tool_call_id);
		pthread_mutex_destroy(&call->mutex);
		free(call);
		return NULL;
	}
	return call;
}

static void async_tool_call_destroy(struct async_tool_call *call)
{
	if (!call)
		return;
	pthread_mutex_destroy(&call->mutex);
	free(call->tool_name);
	free(call->tool_args);
	free(call->tool_call_id);
	free(call->result);
	free(call);
}

static int react_tool_call_cancelled(struct react_context *ctx,
				     const struct async_tool_call *call)
{
	if (!call)
		return 0;
	return react_emit_tool_event(ctx, "tool.cancelled", "cancelled",
				     "tool execution cancelled",
				     call->tool_name, call->tool_args,
				     call->tool_call_id, NULL, -ECANCELED);
}

static int react_tool_call_finish(struct react_context *ctx,
				  const struct async_tool_call *call,
				  const char *result, int rc)
{
	if (!call)
		return 0;
	return react_emit_tool_event(ctx,
				     rc < 0 ? "tool.failed" : "tool.result",
				     rc < 0 ? "failed" : "end",
				     rc < 0 ? "tool failed" : "tool result",
				     call->tool_name, call->tool_args,
				     call->tool_call_id, result, rc);
}

/*
 * Wait for a tool worker to finish.
 *
 * When the cancel flag is not set, joins normally and returns 0.
 *
 * On cancellation, ownership of `call` is transferred to the worker
 * thread by setting `detached = 1` under the mutex, then detaching the
 * thread. Caller must NOT touch `call` afterwards; the worker will free
 * it itself when it exits.
 *
 * Returns 0 if joined normally, -ECANCELED if detached.
 */
static int join_tool_thread(pthread_t thread, volatile sig_atomic_t *cancelled,
			    struct async_tool_call *call)
{
	if (!cancelled || !*cancelled) {
		pthread_join(thread, NULL);
		return 0;
	}

	pthread_mutex_lock(&call->mutex);
	call->cancelled = 1;
	morph_cancel_token_cancel(&call->cancel_token);
	call->detached = 1;
	pthread_mutex_unlock(&call->mutex);
	pthread_detach(thread);
	return -ECANCELED;
}

static void *async_tool_exec(void *arg)
{
	struct async_tool_call *call = (struct async_tool_call *)arg;
	if (!call)
		return NULL;

	http_set_cancel_flag(&call->cancelled);
	http_set_cancel_token(&call->cancel_token);

	char action_buf[512];
	snprintf(action_buf, sizeof(action_buf), "Executing %s...", call->tool_name);
	if (call->output_cb)
		call->output_cb(REACT_STEP_ACTION, action_buf, call->output_user_data);

	int notify_done = 0;

	if (tool_is_disabled(call->tools, call->tool_name)) {
		char disabled_msg[256];
		snprintf(disabled_msg, sizeof(disabled_msg),
			 "tool error: '%s' is disabled in configuration",
			 call->tool_name);

		pthread_mutex_lock(&call->mutex);
		call->result = strdup(disabled_msg);
		call->rc = -EPERM;
		call->completed = 1;
		pthread_mutex_unlock(&call->mutex);
		notify_done = 1;
	} else {
		struct tool_result res;
		void *prev_usage_user_data;
		int rc;

		tool_result_init(&res);
		prev_usage_user_data = model_get_usage_user_data();
		model_set_usage_user_data(call->usage_user_data);
		rc = tool_exec(call->tools, call->tool_name,
			       call->tool_args, &res);
		model_set_usage_user_data(prev_usage_user_data);

		pthread_mutex_lock(&call->mutex);
		int was_cancelled = call->cancelled;
		if (was_cancelled) {
			call->completed = 1;
		} else if (rc < 0) {
			const char *raw = res.text.data ? res.text.data : "unknown error";
			size_t need = strlen(raw) + 64;
			char *buf = malloc(need);
			if (buf)
				snprintf(buf, need, "tool error: %s (%s)",
					 raw, morph_strerror(rc));
			call->result = buf;
			call->rc = rc;
			call->completed = 1;
		} else {
			const char *raw = res.text.data ? res.text.data : "(no output)";
			call->result = utf8_dup_clamped(raw, 256 * 1024);
			call->rc = 0;
			call->completed = 1;
		}
		pthread_mutex_unlock(&call->mutex);

		tool_result_cleanup(&res);
		notify_done = !was_cancelled;
	}

	http_set_cancel_flag(NULL);
	http_set_cancel_token(NULL);

	if (notify_done) {
		char done_buf[512];
		snprintf(done_buf, sizeof(done_buf),
			 "%s completed", call->tool_name);
		if (call->output_cb)
			call->output_cb(REACT_STEP_ACTION, done_buf,
					call->output_user_data);
	}

	pthread_mutex_lock(&call->mutex);
	int detached = call->detached;
	pthread_mutex_unlock(&call->mutex);
	if (detached)
		async_tool_call_destroy(call);
	return NULL;
}

static int summarize_cb(const char *text, void *user_data, char **out)
{
	struct react_context *ctx = user_data;
	struct model *llm = ctx->llm_model;
	if (!llm || !llm->chat || !llm->api_key[0]) {
		*out = strdup(text);
		return *out ? 0 : -ENOMEM;
	}
	const char *sys = "You are a conversation summarizer. "
		"Summarize the following conversation concisely, "
		"preserving all file paths, generated outputs, errors, "
		"and key decisions made. Use 2-4 sentences.";
	const char *msgs[] = { text };
	morph_buf_t b;
	int rc = morph_buf_init(&b, 8192);
	if (rc != 0) {
		*out = strdup(text);
		return *out ? 0 : -ENOMEM;
	}
	rc = llm->chat(llm, ctx->turn_arena, sys, msgs, 1, morph_buf_append_cb, &b);
	if (rc < 0) {
		morph_buf_cleanup(&b);
		*out = strdup(text);
		return *out ? 0 : -ENOMEM;
	}
	*out = morph_buf_detach(&b);
	if (!*out) {
		*out = strdup(text);
		return *out ? 0 : -ENOMEM;
	}
	return 0;
}

const char *react_step_type_name(enum react_step_type type)
{
	switch (type) {
	case REACT_STEP_THOUGHT:	return "Thought";
	case REACT_STEP_ACTION:		return "Action";
	case REACT_STEP_OBSERVATION:	return "Observation";
	case REACT_STEP_REFLECTION:	return "Reflection";
	case REACT_STEP_FINAL:		return "Final";
	case REACT_STEP_REASONING:	return "Reasoning";
	default:			return "Unknown";
	}
}

const char *react_state_name(enum react_state state)
{
	switch (state) {
	case REACT_STATE_INIT:		return "INIT";
	case REACT_STATE_THINKING:	return "THINKING";
	case REACT_STATE_ACTING:	return "ACTING";
	case REACT_STATE_OBSERVING:	return "OBSERVING";
	case REACT_STATE_GUARDRAIL:	return "GUARDRAIL";
	case REACT_STATE_FINAL:		return "FINAL";
	case REACT_STATE_DONE:		return "DONE";
	case REACT_STATE_ABORT:		return "ABORT";
	case REACT_STATE_TOOL_FAIL:	return "TOOL_FAIL";
	default:			return "Unknown";
	}
}

const char *react_outcome_name(enum react_outcome outcome)
{
	switch (outcome) {
	case REACT_OUTCOME_NONE:		return "none";
	case REACT_OUTCOME_SUCCESS:	return "success";
	case REACT_OUTCOME_CANCELLED:	return "cancelled";
	case REACT_OUTCOME_TIMEOUT:	return "timeout";
	case REACT_OUTCOME_MAX_ITERATIONS:
		return "max_iterations";
	case REACT_OUTCOME_LLM_ERROR:	return "llm_error";
	case REACT_OUTCOME_TOOL_ERROR:	return "tool_error";
	case REACT_OUTCOME_GUARDRAIL_DENIED:
		return "guardrail_denied";
	case REACT_OUTCOME_INTERNAL_ERROR:
		return "internal_error";
	default:			return "unknown";
	}
}

struct react_context *react_context_create(struct tool_registry *tools,
					   struct tokenizer *tok,
					   struct compress_config *cfg,
					   struct guardrail_config *gcfg)
{
	struct react_context *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;
	ctx->tools = tools;
	ctx->tokenizer = tok;
	ctx->max_iterations = 10;
	ctx->step_timeout_seconds = 330;
	ctx->tool_max_retries = 3;
	ctx->empty_round_count = 0;
	ctx->guardrail_retry_count = 0;
	react_set_state(ctx, REACT_STATE_INIT);
	ctx->outcome = REACT_OUTCOME_NONE;
	ctx->last_error_code = 0;
	ctx->outcome_reason[0] = '\0';
	ctx->cancelled = 0;
	morph_cancel_token_reset(&ctx->cancel_token);
	ctx->turn_arena = arena_create(0);
	ctx->session_arena = arena_create(0);
	if (!ctx->turn_arena || !ctx->session_arena) {
		arena_destroy(ctx->turn_arena);
		arena_destroy(ctx->session_arena);
		free(ctx);
		return NULL;
	}
	if (cfg)
		ctx->compress = *cfg;
	ctx->compress.summarize = summarize_cb;
	ctx->compress.summarize_user_data = ctx;
	if (gcfg)
		ctx->guardrail = *gcfg;
	else {
		ctx->guardrail.enabled = 0;
		ctx->guardrail.max_retries = 1;
		ctx->guardrail.max_empty_rounds = 2;
	}
	if (ctx->guardrail.rule_count == 0)
		guardrail_register_builtin_rules(&ctx->guardrail);
	ctx->hitl.enabled = 0;
	ctx->hitl.tools_count = 0;
	ctx->hitl.auto_approve_readonly = 1;
	ctx->hitl.approval_cb = NULL;
	ctx->hitl.approval_user_data = NULL;
	ctx->hitl.auto_approved_count = 0;
	return ctx;
}

void react_context_destroy(struct react_context *ctx)
{
	if (!ctx)
		return;
	react_reset(ctx);
	free(ctx->final_answer);
	free(ctx->system_prompt);
	free(ctx->memory_context);
	free(ctx->workdir);
	arena_destroy(ctx->turn_arena);
	if (ctx->session_arena)
		arena_destroy(ctx->session_arena);
	free(ctx);
}

void react_reset(struct react_context *ctx)
{
	if (!ctx)
		return;
	struct react_step *cur = ctx->steps;
	while (cur) {
		struct react_step *next = cur->next;
		react_step_destroy(cur);
		cur = next;
	}
	ctx->steps = NULL;
	ctx->step_count = 0;
	react_set_state(ctx, REACT_STATE_INIT);
	ctx->outcome = REACT_OUTCOME_NONE;
	ctx->last_error_code = 0;
	ctx->outcome_reason[0] = '\0';
	free(ctx->final_answer);
	ctx->final_answer = NULL;
	ctx->tool_fail_name = NULL;
	ctx->tool_fail_args = NULL;
	ctx->tool_fail_count = 0;
	ctx->guardrail_retry_count = 0;
	ctx->empty_round_count = 0;
	ctx->cancelled = 0;
	morph_cancel_token_reset(&ctx->cancel_token);
}

void react_cancel(struct react_context *ctx)
{
	if (ctx) {
		ctx->cancelled = 1;
		morph_cancel_token_cancel(&ctx->cancel_token);
	}
}

int react_set_memory_context(struct react_context *ctx,
			     const char *memory_context)
{
	char *dup = NULL;

	if (!ctx)
		return -EINVAL;
	if (memory_context) {
		dup = strdup(memory_context);
		if (!dup)
			return -ENOMEM;
	}
	free(ctx->memory_context);
	ctx->memory_context = dup;
	return 0;
}

struct react_step *react_step_create(struct arena *arena,
				     enum react_step_type type,
				     const char *content,
				     const char *tool_name,
				     const char *tool_args,
				     const char *tool_call_id)
{
	struct react_step *s = arena_alloc(arena, sizeof(*s));
	if (!s)
		return NULL;
	s->type = type;
	s->content = content ? arena_strdup(arena, content) : NULL;
	s->tool_name = tool_name ? arena_strdup(arena, tool_name) : NULL;
	s->tool_args = tool_args ? arena_strdup(arena, tool_args) : NULL;
	s->tool_call_id = tool_call_id ? arena_strdup(arena, tool_call_id) : NULL;
	s->next = NULL;
	return s;
}

void react_step_destroy(struct react_step *step)
{
	if (!step)
		return;
	/* If we're using arena, we don't free individual fields;
	 * otherwise, free them
	 */
	/* Since we can't know for sure, we just free the step, but
	 * actually, the only place without arena is cli.c's free_json_react_steps,
	 * which does its own cleanup
	 */
	(void)step;
}

static void add_step(struct react_context *ctx, struct react_step *step)
{
	if (!step)
		return;
	if (!ctx->steps) {
		ctx->steps = step;
	} else {
		struct react_step *cur = ctx->steps;
		while (cur->next)
			cur = cur->next;
		cur->next = step;
	}
	ctx->step_count++;
}

static char *build_system_prompt(struct react_context *ctx, struct arena *arena)
{
	morph_buf_t buf;
	int rc = morph_buf_init_arena(&buf, arena, 8192);
	if (rc != 0)
		return NULL;

	char time_buf[128];
	{
		time_t now = time(NULL);
		struct tm tm_local;
		localtime_r(&now, &tm_local);
		strftime(time_buf, sizeof(time_buf),
			 "%Y-%m-%d %A %H:%M:%S %Z", &tm_local);
	}

	rc = morph_buf_printf(&buf, MORPH_SYSTEM_PROMPT, time_buf,
			      ctx->max_iterations);
	if (rc != 0)
		return NULL;

	rc = morph_buf_puts(&buf, MORPH_MARKDOWN_OUTPUT_PROMPT);
	if (rc != 0)
		return NULL;

	if (ctx->workdir && *ctx->workdir) {
		rc = morph_buf_printf(&buf, "\nWorking directory: %s\n",
				      ctx->workdir);
		if (rc != 0)
			return NULL;
	}

	if (ctx->system_prompt) {
		rc = morph_buf_printf(&buf, "%s\n", ctx->system_prompt);
		if (rc != 0)
			return NULL;
	}

	if (ctx->memory_context && ctx->memory_context[0]) {
		rc = morph_buf_printf(&buf, "\n%s\n", ctx->memory_context);
		if (rc != 0)
			return NULL;
	}

	if (ctx->skills && ctx->skills->count > 0) {
		rc = morph_buf_puts(&buf, "\nAvailable skills:\n");
		if (rc != 0)
			return NULL;
		for (int i = 0; i < ctx->skills->count; i++) {
			if (!ctx->skills->entries[i].enabled)
				continue;
			rc = morph_buf_printf(&buf, "- %s: %s\n",
					      ctx->skills->entries[i].fm.name,
					      ctx->skills->entries[i].fm.description);
			if (rc != 0)
				return NULL;
		}
		rc = morph_buf_puts(&buf,
			"\nWhen a skill matches the task, call activate_skill "
			"with the skill name to load its full instructions.\n");
		if (rc != 0)
			return NULL;
	}

	if (ctx->sub_agent_info && ctx->sub_agent_info_count > 0) {
		rc = morph_buf_puts(&buf, "\nAvailable sub-agents:\n");
		if (rc != 0)
			return NULL;
		for (int i = 0; i < ctx->sub_agent_info_count; i++) {
			rc = morph_buf_printf(&buf, "- agent_%s: %s\n",
					      ctx->sub_agent_info[i].name,
					      ctx->sub_agent_info[i].description);
			if (rc != 0)
				return NULL;
		}
		rc = morph_buf_puts(&buf,
			"\nTo delegate a task, call agent_<name> with a task "
			"description. For parallel execution, use fanout. "
			"For async delegation, use delegate + agent_status.\n");
		if (rc != 0)
			return NULL;
	}

	if (ctx->ask_user_fn) {
		rc = morph_buf_puts(&buf,
			"\nYou have the ask_user tool. Use it ONLY for genuine "
			"ambiguity or irreversible decisions. Prefer acting on "
			"reasonable assumptions rather than blocking for input.\n");
		if (rc != 0)
			return NULL;
	}

	if (ctx->skills) {
		char *active = skill_build_activated_instructions(ctx->skills);
		if (active) {
			rc = morph_buf_puts(&buf, active);
			free(active);
			if (rc != 0)
				return NULL;
		}
	}

	return buf.data;
}

struct react_stream_data {
	struct react_context *ctx;
	react_output_cb user_cb;
	void *user_data;
	volatile sig_atomic_t *cancelled;
	struct arena *arena;
	char *accumulated;
	size_t acc_len;
	size_t acc_cap;
};

static int react_stream_cb(const char *token, void *user_data)
{
	struct react_stream_data *sd = user_data;
	size_t tlen = strlen(token);
	if (sd->acc_len + tlen + 1 >= sd->acc_cap) {
		size_t new_cap = (sd->acc_len + tlen + 1) * 2;
		char *new_acc = arena_alloc(sd->arena, new_cap);
		if (new_acc) {
			if (sd->accumulated) {
				memcpy(new_acc, sd->accumulated, sd->acc_len);
			}
			sd->accumulated = new_acc;
			sd->acc_cap = new_cap;
		}
	}
	if (sd->accumulated && sd->acc_len + tlen < sd->acc_cap) {
		memcpy(sd->accumulated + sd->acc_len, token, tlen);
		sd->acc_len += tlen;
		sd->accumulated[sd->acc_len] = '\0';
	}
	if (react_sigint_flag) {
		if (sd->cancelled)
			*sd->cancelled = 1;
		react_sigint_flag = 0;
	}
	if (sd->cancelled && *sd->cancelled)
		return -EINTR;
	if (sd->user_cb)
		sd->user_cb(REACT_STEP_THOUGHT, token, sd->user_data);
	react_emit_text_event(sd->ctx, MORPH_EVENT_REACT,
			      "react.thought.delta", "delta",
			      NULL, token);
	return 0;
}

static int react_typed_stream_cb(enum llm_stream_kind kind, const char *token,
				 void *user_data)
{
	struct react_stream_data *sd = user_data;

	if (kind == LLM_STREAM_REASONING) {
		if (react_sigint_flag) {
			if (sd->cancelled)
				*sd->cancelled = 1;
			react_sigint_flag = 0;
		}
		if (sd->cancelled && *sd->cancelled)
			return -EINTR;
		if (sd->user_cb)
			sd->user_cb(REACT_STEP_REASONING, token,
				    sd->user_data);
		react_emit_text_event(sd->ctx, MORPH_EVENT_REACT,
				      "react.reasoning.delta", "delta",
				      NULL, token);
		return 0;
	}
	return react_stream_cb(token, user_data);
}

static int count_active_tools(struct tool_registry *reg)
{
	int count = 0;
	for (int i = 0; i < reg->count; i++) {
		if (!tool_is_disabled(reg, reg->entries[i].desc.name))
			count++;
	}
	return count;
}

static void collect_active_tools(struct tool_registry *reg,
				 struct tool_desc *out, int max_count)
{
	int idx = 0;
	for (int i = 0; i < reg->count && idx < max_count; i++) {
		if (!tool_is_disabled(reg, reg->entries[i].desc.name)) {
			out[idx] = reg->entries[i].desc;
			idx++;
		}
	}
}

static struct chat_message *react_push_chat_message(morph_array_t *messages)
{
	struct chat_message *m;

	m = morph_array_push(messages);
	if (!m)
		return NULL;
	memset(m, 0, sizeof(*m));
	return m;
}

static int react_append_tool_message(morph_array_t *messages,
				     const char *content,
				     const char *tool_call_id,
				     struct arena *arena)
{
	struct chat_message *m;

	m = react_push_chat_message(messages);
	if (!m)
		return -ENOMEM;
	m->role = arena_strdup(arena, "tool");
	m->content = arena_strdup(arena, content);
	m->tool_call_id = arena_strdup(arena, tool_call_id);
	m->tool_calls = NULL;
	m->tool_call_count = 0;
	return 0;
}

/*
 * Check HITL approval for each tool call in a response.
 *
 * Iterates over tool calls and queries the approval callback for any
 * tool that requires human approval.  Denied tools are recorded in
 * slots and their action/observation steps are added to the
 * ReAct trace.
 *
 * ctx - ReAct context (HITL config and step list)
 * response - LLM response containing tool calls
 * slots - Output array (one slot per tool call)
 * cb - Output callback for step notifications
 * user_data - Opaque pointer passed to cb
 */
static void react_check_hitl_approvals(struct react_context *ctx,
					struct chat_response *response,
					struct react_tool_slot *slots,
					react_output_cb cb,
					void *user_data)
{
	(void)cb;
	(void)user_data;
	for (int i = 0; i < response->tool_call_count; i++) {
		struct tool_call *tc = &response->tool_calls[i];
		const char *tool_name = tc->name;
		const char *tool_args = tc->arguments ? tc->arguments : "{}";
		if (!hitl_needs_approval(ctx, tool_name))
			continue;
		if (!ctx->hitl.approval_cb)
			continue;
		react_emit_hitl_event(ctx, "hitl.request", "begin",
				      "approval requested", tool_name,
				      tool_args, tc->id, NULL);
		enum hitl_verdict v = ctx->hitl.approval_cb(
			tool_name, tool_args, ctx->hitl.approval_user_data);
		if (v == HITL_ALWAYS) {
			hitl_add_auto_approved(&ctx->hitl, tool_name);
			react_emit_hitl_event(ctx, "hitl.always", "end",
					      "approval persisted",
					      tool_name, tool_args, tc->id,
					      "always");
		} else if (v == HITL_DENY) {
			slots[i].hitl_denied = 1;
			react_emit_hitl_event(ctx, "hitl.denied", "failed",
					      "approval denied", tool_name,
					      tool_args, tc->id, "denied");
			char deny_msg[512];
			snprintf(deny_msg, sizeof(deny_msg),
				 "tool error: '%s' execution denied by user",
				 tool_name);
			size_t at_len = strlen(tool_name) + strlen(tool_args) + 4;
			char *action_text = arena_alloc(ctx->turn_arena, at_len);
			if (action_text)
				snprintf(action_text, at_len,
					 "%s(%s)", tool_name, tool_args);
			struct react_step *action = react_step_create(
				ctx->turn_arena, REACT_STEP_ACTION,
				action_text ? action_text : "",
				tool_name, tool_args, tc->id);
			add_step(ctx, action);
			struct react_step *obs = react_step_create(
				ctx->turn_arena, REACT_STEP_OBSERVATION,
				deny_msg, NULL, NULL, NULL);
			add_step(ctx, obs);
			react_emit_text_event(ctx, MORPH_EVENT_REACT,
					      "react.observation", "failed",
					      "tool execution denied",
					      deny_msg);
		} else {
			react_emit_hitl_event(ctx, "hitl.approved", "end",
					      "approval granted", tool_name,
					      tool_args, tc->id, "approved");
		}
	}
}

/*
 * Track consecutive tool failures and force a Final step if the retry
 * limit is exceeded.
 *
 * On success (rc >= 0), the failure counters are reset.
 * On failure (rc < 0), the counters are updated and checked against
 * ctx->tool_max_retries.
 *
 * Returns 1 if max retries reached (caller should abort the loop),
 * 0 otherwise.
 */
static int react_track_tool_failure(struct react_context *ctx,
				     const char *tool_name,
				     const char *tool_args,
				     int rc,
				     react_output_cb cb,
				     void *user_data)
{
	if (rc < 0) {
		react_set_state(ctx, REACT_STATE_TOOL_FAIL);
		if (ctx->tool_fail_name &&
		    strcmp(tool_name, ctx->tool_fail_name) == 0 &&
		    ctx->tool_fail_args &&
		    strcmp(tool_args, ctx->tool_fail_args) == 0) {
			ctx->tool_fail_count++;
		} else {
			ctx->tool_fail_name = arena_strdup(ctx->turn_arena, tool_name);
			ctx->tool_fail_args = arena_strdup(ctx->turn_arena, tool_args);
			ctx->tool_fail_count = 1;
		}
	} else {
		ctx->tool_fail_name = NULL;
		ctx->tool_fail_args = NULL;
		ctx->tool_fail_count = 0;
	}
	if (ctx->tool_fail_count < ctx->tool_max_retries)
		return 0;
	const char *fail_name = ctx->tool_fail_name ? ctx->tool_fail_name : "(unknown)";
	log_warn("react_run: tool '%s' failed %d times consecutively, forcing Final",
		 fail_name, ctx->tool_fail_count);
	char fail_msg[256];
	snprintf(fail_msg, sizeof(fail_msg),
		 "Tool '%s' repeatedly failed. Please try a different approach.",
		 fail_name);
	struct react_step *final_step = react_step_create(ctx->turn_arena,
		REACT_STEP_FINAL, fail_msg, NULL, NULL, NULL);
	add_step(ctx, final_step);
	free(ctx->final_answer);
	ctx->final_answer = strdup(fail_msg);
	react_set_state(ctx, REACT_STATE_DONE);
	if (cb)
		cb(REACT_STEP_FINAL, fail_msg, user_data);
	react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.final",
			      "end", "final answer", fail_msg);
	ctx->tool_fail_name = NULL;
	ctx->tool_fail_args = NULL;
	ctx->tool_fail_count = 0;
	return 1;
}

/*
 * Evaluate the guardrail for a proposed final answer.
 *
 * Returns 0 guardrail passed (caller should finalize),
 *         1 guardrail failed - revision messages appended, caller should
 *           free response and continue the iteration loop,
 *        -ENOMEM allocation failure (caller should abort).
 */
static int react_handle_guardrail_retry(struct react_context *ctx,
					const char *proposed,
					morph_array_t *messages,
					react_output_cb cb,
					void *user_data)
{
	struct chat_message *slots;
	struct chat_message *asst_msg;
	struct chat_message *user_msg;

	if (!ctx->guardrail.enabled ||
	    ctx->guardrail_retry_count >= ctx->guardrail.max_retries)
		return 0;

	if (!proposed || !*proposed ||
	    strcmp(proposed, "(no response)") == 0) {
		ctx->empty_round_count++;
	} else {
		ctx->empty_round_count = 0;
	}

	react_set_state(ctx, REACT_STATE_GUARDRAIL);

	struct guardrail_eval_ctx eval = {
		.proposed_answer = proposed,
		.steps = ctx->steps,
		.empty_round_count = ctx->empty_round_count,
		.arena = ctx->turn_arena,
	};
	struct guardrail_result gr = guardrail_run_hook(
		&ctx->guardrail, GUARDRAIL_HOOK_OUTPUT, &eval);

	if (gr.verdict == GUARDRAIL_PASS) {
		log_info("guardrail: PASS");
		return 0;
	}

	ctx->guardrail_retry_count++;
	log_info("guardrail: %s (attempt %d/%d)",
		 gr.reason, ctx->guardrail_retry_count,
		 ctx->guardrail.max_retries);

	struct react_step *refl_step = react_step_create(ctx->turn_arena,
		REACT_STEP_REFLECTION, gr.reason, NULL, NULL, NULL);
	add_step(ctx, refl_step);
	if (cb)
		cb(REACT_STEP_REFLECTION, gr.reason, user_data);
	react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.reflection",
			      "failed", "guardrail reflection", gr.reason);

	slots = morph_array_push_n(messages, 2);
	if (!slots)
		return -ENOMEM;
	memset(slots, 0, 2 * sizeof(*slots));
	asst_msg = &slots[0];
	user_msg = &slots[1];

	asst_msg->role = arena_strdup(ctx->turn_arena, "assistant");
	asst_msg->content = arena_strdup(ctx->turn_arena, proposed);
	asst_msg->tool_call_id = NULL;
	asst_msg->tool_calls = NULL;
	asst_msg->tool_call_count = 0;

	const char *action = (gr.triggered_rule &&
			      gr.triggered_rule->action_text[0])
		? gr.triggered_rule->action_text
		: "Try again using the available tools.";

	size_t rev_cap = strlen(gr.reason) + strlen(action) + 64;
	char *rev_msg = arena_alloc(ctx->turn_arena, rev_cap);
	if (rev_msg) {
		snprintf(rev_msg, rev_cap,
			 "Quality check failed: %s\n%s",
			 gr.reason, action);
	}
	user_msg->role = arena_strdup(ctx->turn_arena, "user");
	user_msg->content = rev_msg ? rev_msg :
		arena_strdup(ctx->turn_arena,
			     "Please revise your answer using the available tools.");
	user_msg->tool_call_id = NULL;
	user_msg->tool_calls = NULL;
	user_msg->tool_call_count = 0;
	struct message_list *ml_asst = msg_list_create(ctx->session_arena, "assistant", proposed,
		tokenizer_count(ctx->tokenizer, proposed));
	msg_list_append(&ctx->messages, ml_asst);
	const char *rev_text = rev_msg ? rev_msg :
		"Please revise your answer using the available tools.";
	struct message_list *ml_user = msg_list_create(ctx->session_arena, "user",
		rev_text, tokenizer_count(ctx->tokenizer, rev_text));
	msg_list_append(&ctx->messages, ml_user);
	return 1;
}

static int react_check_input_guardrail(struct react_context *ctx,
				       const char *user_input)
{
	struct guardrail_eval_ctx eval;
	struct guardrail_result gr;
	const char *action;
	char msg[2048];
	struct react_step *refl;

	if (!ctx->guardrail.enabled)
		return 0;

	memset(&eval, 0, sizeof(eval));
	eval.user_input = user_input;
	eval.steps = NULL;
	eval.arena = ctx->turn_arena;
	gr = guardrail_run_hook(&ctx->guardrail, GUARDRAIL_HOOK_INPUT, &eval);
	if (gr.verdict != GUARDRAIL_FAIL)
		return 0;

	log_info("guardrail[input]: %s", gr.reason);
	action = (gr.triggered_rule && gr.triggered_rule->action_text[0])
		? gr.triggered_rule->action_text : "";
	snprintf(msg, sizeof(msg), "Input rejected: %s\n%s",
		 gr.reason, action);
	refl = react_step_create(ctx->turn_arena, REACT_STEP_REFLECTION,
				 msg, NULL, NULL, NULL);
	add_step(ctx, refl);
	react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.reflection",
			      "failed", "input rejected", msg);
	free(ctx->final_answer);
	ctx->final_answer = strdup(msg);
	react_set_result(ctx, REACT_OUTCOME_GUARDRAIL_DENIED,
			 -EPERM, "guardrail_denied");
	return 1;
}

static void react_run_without_llm(struct react_context *ctx,
				  const char *user_input,
				  react_output_cb cb, void *user_data)
{
	struct react_step *thought;
	struct react_step *final_step;

	thought = react_step_create(ctx->turn_arena, REACT_STEP_THOUGHT,
				    "Processing user input...",
				    NULL, NULL, NULL);
	add_step(ctx, thought);
	if (cb)
		cb(REACT_STEP_THOUGHT, "Processing user input...", user_data);

	free(ctx->final_answer);
	ctx->final_answer = strdup(user_input);

	final_step = react_step_create(ctx->turn_arena, REACT_STEP_FINAL,
				       user_input, NULL, NULL, NULL);
	add_step(ctx, final_step);
	if (cb)
		cb(REACT_STEP_FINAL, user_input, user_data);
	react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.final",
			      "end", "final answer", user_input);
	react_set_result(ctx, REACT_OUTCOME_SUCCESS, 0, NULL);
}

static int react_prepare_active_tools(struct react_context *ctx,
				      struct tool_desc **active_tools,
				      int *active_tool_count)
{
	int has_tools;

	if (!active_tools || !active_tool_count)
		return -EINVAL;
	*active_tools = NULL;
	*active_tool_count = 0;
	has_tools = ctx->tools && ctx->tools->count > 0;
	*active_tool_count = has_tools ? count_active_tools(ctx->tools) : 0;
	if (*active_tool_count <= 0)
		return 0;
	*active_tools = arena_alloc(ctx->turn_arena,
				    (size_t)*active_tool_count *
				    sizeof(**active_tools));
	if (!*active_tools)
		return -ENOMEM;
	collect_active_tools(ctx->tools, *active_tools, *active_tool_count);
	return 0;
}

static int react_prepare_messages(struct react_context *ctx,
				  morph_array_t *messages)
{
	struct message_list *hist;
	struct chat_message *message;

	if (!messages)
		return -EINVAL;
	if (morph_array_init(messages, 64, sizeof(struct chat_message)) < 0)
		return -ENOMEM;

	hist = ctx->messages;
	while (hist) {
		message = react_push_chat_message(messages);
		if (!message)
			return -ENOMEM;
		message->role = arena_strdup(ctx->turn_arena, hist->role);
		message->content = hist->content ?
			arena_strdup(ctx->turn_arena, hist->content) :
			arena_strdup(ctx->turn_arena, "");
		message->tool_call_id = NULL;
		message->tool_calls = NULL;
		message->tool_call_count = 0;
		hist = hist->next;
	}
	return 0;
}

static void react_complete_max_iterations(struct react_context *ctx)
{
	struct react_step *last_obs = NULL;
	struct react_step *cur;

	if (ctx->state == REACT_STATE_DONE || ctx->state == REACT_STATE_ABORT)
		return;

	log_warn("react_run: max iterations (%d) reached, aborting",
		 ctx->max_iterations);
	if (!ctx->final_answer) {
		cur = ctx->steps;
		while (cur) {
			if (cur->type == REACT_STEP_OBSERVATION)
				last_obs = cur;
			cur = cur->next;
		}
		if (last_obs && last_obs->content) {
			ctx->final_answer = strdup(last_obs->content);
		} else {
			ctx->final_answer = strdup(
				"Maximum iterations reached. "
				"No final answer produced.");
		}
	}
	react_set_result(ctx, REACT_OUTCOME_MAX_ITERATIONS,
			 MORPH_ERR_REACT_MAX_ITERATIONS,
			 "max_iterations");
}

static void react_append_final_message(struct react_context *ctx)
{
	const char *answer;
	struct message_list *asst;

	if (ctx->state != REACT_STATE_DONE || !ctx->steps)
		return;
	answer = ctx->final_answer ? ctx->final_answer : "(no answer)";
	asst = msg_list_create(ctx->session_arena, "assistant", answer,
			       tokenizer_count(ctx->tokenizer, answer));
	msg_list_append(&ctx->messages, asst);
}

static int react_chat_once(struct react_context *ctx, struct model *llm,
			   const char *system_prompt,
			   struct chat_message *messages, int msg_count,
			   struct tool_desc *active_tools,
			   int active_tool_count,
			   react_output_cb cb, void *user_data,
			   struct chat_response *response,
			   time_t *llm_start, time_t *llm_end)
{
	struct react_stream_data sd;
	int status;

	memset(&sd, 0, sizeof(sd));
	sd.ctx = ctx;
	sd.user_cb = cb;
	sd.user_data = user_data;
	sd.cancelled = &ctx->cancelled;
	sd.arena = ctx->turn_arena;
	sd.accumulated = arena_alloc(ctx->turn_arena, 8192);
	sd.acc_len = 0;
	sd.acc_cap = 8192;
	if (sd.accumulated)
		sd.accumulated[0] = '\0';

	*llm_start = time(NULL);
	if (llm->chat_with_tools_stream) {
		status = llm->chat_with_tools_stream(
			llm, ctx->turn_arena, system_prompt, messages,
			msg_count, active_tools, active_tool_count, response,
			react_typed_stream_cb, &sd);
	} else if (llm->chat_with_tools) {
		status = llm->chat_with_tools(llm, ctx->turn_arena,
					      system_prompt, messages, msg_count,
					      active_tools, active_tool_count,
					      response, react_stream_cb, &sd);
	} else {
		int hist_n = 0;
		struct message_list *h = ctx->messages;
		const char **hist_msgs;

		while (h) {
			hist_n++;
			h = h->next;
		}
		hist_msgs = arena_alloc(ctx->turn_arena,
					(size_t)hist_n * sizeof(*hist_msgs));
		if (!hist_msgs && hist_n > 0) {
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
					 -ENOMEM, "internal_error");
			*llm_end = time(NULL);
			return -ENOMEM;
		}
		if (hist_msgs) {
			h = ctx->messages;
			for (int i = 0; i < hist_n && h; i++) {
				hist_msgs[i] = h->content ? h->content : "";
				h = h->next;
			}
		}
		status = llm->chat(llm, ctx->turn_arena, system_prompt,
				   hist_msgs, hist_n, react_stream_cb, &sd);
		if (status >= 0 && sd.accumulated) {
			response->content = sd.accumulated;
			sd.accumulated = NULL;
			response->arena = ctx->turn_arena;
		}
	}
	*llm_end = time(NULL);
	return status;
}

static int react_handle_llm_cancelled(struct react_context *ctx,
				      struct chat_response *response)
{
	struct react_step *obs;

	if (!ctx->cancelled)
		return 0;
	log_info("react_run: cancelled during LLM call");
	obs = react_step_create(ctx->turn_arena, REACT_STEP_OBSERVATION,
				"LLM call interrupted by user",
				NULL, NULL, NULL);
	add_step(ctx, obs);
	react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.observation",
			      "cancelled", "LLM call interrupted",
			      "LLM call interrupted by user");
	if (response->content) {
		free(ctx->final_answer);
		ctx->final_answer = strdup(response->content);
	}
	react_set_result(ctx, REACT_OUTCOME_CANCELLED, -ECANCELED,
			 "user_cancelled");
	return 1;
}

static int react_handle_llm_error(struct react_context *ctx,
				  struct chat_response *response,
				  int status, react_output_cb cb,
				  void *user_data)
{
	const char *err_content = "LLM call failed";
	struct react_step *err;

	if (status >= 0)
		return 0;
	log_err("react_run: LLM call failed: %d", status);
	if (response->content && *response->content)
		err_content = response->content;
	err = react_step_create(ctx->turn_arena, REACT_STEP_OBSERVATION,
				err_content, NULL, NULL, NULL);
	add_step(ctx, err);
	if (cb)
		cb(REACT_STEP_OBSERVATION, err_content, user_data);
	react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.observation",
			      "failed", "LLM call failed", err_content);
	react_set_result(ctx, REACT_OUTCOME_LLM_ERROR, status, "llm_error");
	return 1;
}

static int react_handle_llm_timeout(struct react_context *ctx,
				    struct chat_response *response,
				    time_t llm_start, time_t llm_end,
				    react_output_cb cb, void *user_data)
{
	long elapsed;
	char timeout_msg[256];
	struct react_step *obs;

	if (ctx->step_timeout_seconds <= 0 ||
	    (llm_end - llm_start) < ctx->step_timeout_seconds)
		return 0;

	elapsed = (long)(llm_end - llm_start);
	log_warn("react_run: LLM call exceeded step timeout (%lds >= %ds)",
		 elapsed, ctx->step_timeout_seconds);
	snprintf(timeout_msg, sizeof(timeout_msg),
		 "LLM call timed out (took %lds, limit %ds)",
		 elapsed, ctx->step_timeout_seconds);
	obs = react_step_create(ctx->turn_arena, REACT_STEP_OBSERVATION,
				timeout_msg, NULL, NULL, NULL);
	add_step(ctx, obs);
	if (cb)
		cb(REACT_STEP_OBSERVATION, timeout_msg, user_data);
	react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.observation",
			      "timeout", "LLM call timed out", timeout_msg);
	free(ctx->final_answer);
	ctx->final_answer = strdup(response->content && *response->content ?
				   response->content : timeout_msg);
	react_set_result(ctx, REACT_OUTCOME_TIMEOUT, -ETIMEDOUT,
			 "step_timeout");
	return 1;
}

static int react_append_assistant_tool_call_message(
	struct react_context *ctx, morph_array_t *messages,
	struct chat_response *response)
{
	struct chat_message *asst_msg;

	asst_msg = react_push_chat_message(messages);
	if (!asst_msg)
		return -ENOMEM;
	asst_msg->role = arena_strdup(ctx->turn_arena, "assistant");
	asst_msg->content = (response->content && *response->content)
		? arena_strdup(ctx->turn_arena, response->content) : NULL;
	asst_msg->tool_calls = arena_alloc(ctx->turn_arena,
		(size_t)response->tool_call_count *
		sizeof(*asst_msg->tool_calls));
	if (!asst_msg->tool_calls)
		return -ENOMEM;
	asst_msg->tool_call_count = response->tool_call_count;
	for (int j = 0; j < response->tool_call_count; j++) {
		strncpy(asst_msg->tool_calls[j].id,
			response->tool_calls[j].id,
			sizeof(asst_msg->tool_calls[j].id) - 1);
		asst_msg->tool_calls[j].id[
			sizeof(asst_msg->tool_calls[j].id) - 1] = '\0';
		strncpy(asst_msg->tool_calls[j].name,
			response->tool_calls[j].name,
			sizeof(asst_msg->tool_calls[j].name) - 1);
		asst_msg->tool_calls[j].name[
			sizeof(asst_msg->tool_calls[j].name) - 1] = '\0';
		asst_msg->tool_calls[j].arguments =
			response->tool_calls[j].arguments
			? arena_strdup(ctx->turn_arena,
				       response->tool_calls[j].arguments)
			: arena_strdup(ctx->turn_arena, "");
	}
	return 0;
}

static void react_start_tool_calls(struct react_context *ctx,
				   struct chat_response *response,
				   struct react_tool_slot *slots,
				   react_output_cb cb, void *user_data)
{
	react_check_hitl_approvals(ctx, response, slots, cb, user_data);

	for (int i = 0; i < response->tool_call_count; i++) {
		struct tool_call *tc;
		const char *tool_name;
		const char *tool_args;
		size_t at_len;
		char *action_text;
		struct react_step *action;

		if (slots[i].hitl_denied)
			continue;

		tc = &response->tool_calls[i];
		tool_name = tc->name;
		tool_args = tc->arguments ? tc->arguments : "{}";
		at_len = strlen(tool_name) + strlen(tool_args) + 4;
		action_text = arena_alloc(ctx->turn_arena, at_len);
		if (action_text)
			snprintf(action_text, at_len, "%s(%s)",
				 tool_name, tool_args);
		action = react_step_create(ctx->turn_arena, REACT_STEP_ACTION,
					   action_text ? action_text : "",
					   tool_name, tool_args, tc->id);
		add_step(ctx, action);
		if (cb)
			cb(REACT_STEP_ACTION, action_text ? action_text : "",
			   user_data);
		react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.action",
				      "begin", "tool action",
				      action_text ? action_text : "");
		react_tool_call_begin(ctx, tc);

		slots[i].call = async_tool_call_create(ctx->tools, ctx,
						       tool_name, tool_args,
						       tc->id, cb, user_data);
		if (!slots[i].call) {
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
					 -ENOMEM, "internal_error");
			break;
		}

		react_tool_call_running(ctx, tc);
		if (pthread_create(&slots[i].thread, NULL, async_tool_exec,
				   slots[i].call) != 0) {
			react_tool_call_thread_failed(ctx, tc, -EAGAIN);
			async_tool_call_destroy(slots[i].call);
			slots[i].call = NULL;
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
					 -EAGAIN, "tool_thread_failed");
			break;
		}
		slots[i].thread_started = 1;
	}
}

static void react_cancel_remaining_tool_calls(struct react_tool_slot *slots,
					      int start, int end)
{
	for (int j = start; j < end; j++) {
		if (slots[j].call && slots[j].thread_started) {
			pthread_mutex_lock(&slots[j].call->mutex);
			slots[j].call->cancelled = 1;
			morph_cancel_token_cancel(
				&slots[j].call->cancel_token);
			slots[j].call->detached = 1;
			pthread_mutex_unlock(&slots[j].call->mutex);
			pthread_detach(slots[j].thread);
			slots[j].call = NULL;
			slots[j].thread_started = 0;
		}
	}
}

static void react_cleanup_tool_calls(struct react_tool_slot *slots, int count)
{
	for (int i = 0; i < count; i++) {
		if (!slots[i].call)
			continue;
		if (!slots[i].call->completed && slots[i].thread_started) {
			pthread_mutex_lock(&slots[i].call->mutex);
			slots[i].call->cancelled = 1;
			morph_cancel_token_cancel(
				&slots[i].call->cancel_token);
			slots[i].call->detached = 1;
			pthread_mutex_unlock(&slots[i].call->mutex);
			pthread_detach(slots[i].thread);
			slots[i].call = NULL;
			slots[i].thread_started = 0;
		} else {
			async_tool_call_destroy(slots[i].call);
			slots[i].call = NULL;
			slots[i].thread_started = 0;
		}
	}
}

static int react_append_denied_tool_message(struct react_context *ctx,
					    struct tool_call *tc,
					    morph_array_t *messages)
{
	return react_append_tool_message(messages,
					 "tool error: execution denied by user",
					 tc->id, ctx->turn_arena);
}

static void react_record_tool_cancelled(struct react_context *ctx)
{
	struct react_step *obs;

	obs = react_step_create(ctx->turn_arena, REACT_STEP_OBSERVATION,
				"Tool execution cancelled by user",
				NULL, NULL, NULL);
	add_step(ctx, obs);
	react_emit_text_event(ctx, MORPH_EVENT_REACT,
			      "react.observation", "cancelled",
			      "tool execution cancelled",
			      "Tool execution cancelled by user");
	react_set_result(ctx, REACT_OUTCOME_CANCELLED, -ECANCELED,
			 "tool_cancelled");
}

static void react_collect_tool_result(struct async_tool_call *call,
				      int *rc, char **result)
{
	pthread_mutex_lock(&call->mutex);
	*rc = call->rc;
	*result = call->result;
	call->result = NULL;
	pthread_mutex_unlock(&call->mutex);
}

static const char *react_apply_tool_output_guardrail(
	struct react_context *ctx, struct async_tool_call *call,
	const char *obs_text, react_output_cb cb, void *user_data)
{
	struct guardrail_eval_ctx geval;
	struct guardrail_result ggr;
	const char *gaction;
	char guard_obs[2048];
	struct react_step *grefl;

	if (!ctx->guardrail.enabled)
		return obs_text;

	memset(&geval, 0, sizeof(geval));
	geval.tool_name = call->tool_name;
	geval.tool_args = call->tool_args;
	geval.tool_result = obs_text;
	geval.steps = ctx->steps;
	geval.arena = ctx->turn_arena;
	ggr = guardrail_run_hook(&ctx->guardrail, GUARDRAIL_HOOK_TOOL_OUTPUT,
				 &geval);
	if (ggr.verdict != GUARDRAIL_FAIL)
		return obs_text;

	log_info("guardrail[tool_output]: %s", ggr.reason);
	gaction = (ggr.triggered_rule &&
		   ggr.triggered_rule->action_text[0])
		? ggr.triggered_rule->action_text
		: "Verify tool parameters and try again.";
	snprintf(guard_obs, sizeof(guard_obs), "guardrail: %s\n%s",
		 ggr.reason, gaction);
	grefl = react_step_create(ctx->turn_arena, REACT_STEP_REFLECTION,
				  ggr.reason, NULL, NULL, NULL);
	add_step(ctx, grefl);
	if (cb)
		cb(REACT_STEP_REFLECTION, ggr.reason, user_data);
	react_emit_text_event(ctx, MORPH_EVENT_REACT,
			      "react.reflection", "failed",
			      "guardrail reflection", ggr.reason);
	return arena_strdup(ctx->turn_arena, guard_obs);
}

static int react_record_tool_observation(struct react_context *ctx,
					 struct async_tool_call *call,
					 const char *obs_text, int rc,
					 morph_array_t *messages,
					 react_output_cb cb, void *user_data)
{
	struct react_step *obs;

	obs = react_step_create(ctx->turn_arena, REACT_STEP_OBSERVATION,
				obs_text, NULL, NULL, NULL);
	add_step(ctx, obs);
	if (cb)
		cb(REACT_STEP_OBSERVATION, obs_text, user_data);
	react_emit_text_event(ctx, MORPH_EVENT_REACT,
			      "react.observation",
			      rc < 0 ? "failed" : "end",
			      "tool observation", obs_text);

	return react_append_tool_message(messages, obs_text,
					 call->tool_call_id, ctx->turn_arena);
}

static int react_poll_cancel(struct react_context *ctx, int iteration);

static int react_join_tool_calls(struct react_context *ctx,
				 struct chat_response *response,
				 struct react_tool_slot *slots,
				 morph_array_t *messages,
				 react_output_cb cb, void *user_data)
{
	int num_tools = response->tool_call_count;

	if (react_sigint_flag) {
		ctx->cancelled = 1;
		react_sigint_flag = 0;
	}

	for (int i = 0; i < num_tools; i++) {
		struct async_tool_call *call;
		int rc;
		char *result;
		const char *obs_text;

		if (slots[i].hitl_denied) {
			struct tool_call *tc = &response->tool_calls[i];
			if (react_append_denied_tool_message(ctx, tc,
							     messages) < 0) {
				react_set_result(ctx,
						 REACT_OUTCOME_INTERNAL_ERROR,
						 -ENOMEM, "internal_error");
				return 1;
			}
			continue;
		}

		if (!slots[i].call)
			continue;

		if (ctx->cancelled) {
			pthread_mutex_lock(&slots[i].call->mutex);
			slots[i].call->cancelled = 1;
			morph_cancel_token_cancel(&slots[i].call->cancel_token);
			pthread_mutex_unlock(&slots[i].call->mutex);
		}

		if (join_tool_thread(slots[i].thread, &ctx->cancelled,
				     slots[i].call) != 0) {
			react_tool_call_cancelled(ctx, slots[i].call);
			slots[i].call = NULL;
			slots[i].thread_started = 0;
			react_record_tool_cancelled(ctx);
			break;
		}
		slots[i].thread_started = 0;

		react_set_state(ctx, REACT_STATE_OBSERVING);
		call = slots[i].call;
		react_collect_tool_result(call, &rc, &result);

		if (react_track_tool_failure(ctx, call->tool_name,
					     call->tool_args, rc, cb,
					     user_data)) {
			react_cancel_remaining_tool_calls(slots, i + 1,
							  num_tools);
			for (int j = 0; j <= i; j++) {
				if (slots[j].call) {
					async_tool_call_destroy(slots[j].call);
					slots[j].call = NULL;
					slots[j].thread_started = 0;
				}
			}
			free(result);
			return 1;
		}

		obs_text = result ? result : "";
		react_tool_call_finish(ctx, call, obs_text, rc);
		if (rc >= 0)
			react_emit_artifacts_from_text(ctx, obs_text,
						       call->tool_name);

		obs_text = react_apply_tool_output_guardrail(ctx, call,
							     obs_text, cb,
							     user_data);
		if (!obs_text)
			obs_text = "";
		if (react_record_tool_observation(ctx, call, obs_text, rc,
						  messages,
						  cb, user_data) < 0) {
			free(result);
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
					 -ENOMEM, "internal_error");
			return 1;
		}
		free(result);
	}

	react_cleanup_tool_calls(slots, num_tools);

	if (react_poll_cancel(ctx, -1))
		return 1;
	return ctx->state == REACT_STATE_DONE ||
		ctx->state == REACT_STATE_ABORT;
}

static int react_poll_cancel(struct react_context *ctx, int iteration)
{
	if (react_sigint_flag) {
		ctx->cancelled = 1;
		morph_cancel_token_cancel(&ctx->cancel_token);
		react_sigint_flag = 0;
	}

	if (ctx->action_drain_fn) {
		struct react_action act;
		int got = ctx->action_drain_fn(ctx->action_drain_user_data,
					       &act, 0);
		if (got > 0 && strcmp(act.type, "cancel") == 0) {
			ctx->cancelled = 1;
			morph_cancel_token_cancel(&ctx->cancel_token);
		}
	}

	if (!ctx->cancelled)
		return 0;

	if (iteration >= 0) {
		log_info("react_run: cancelled by user at iteration %d",
			 iteration);
	}
	react_set_result(ctx, REACT_OUTCOME_CANCELLED, -ECANCELED,
			 "user_cancelled");
	return 1;
}

static void react_maybe_compress_context(struct react_context *ctx)
{
	struct compress_result cr = {0};
	int rc;

	if (!context_needs_compress(ctx->messages, ctx->tokenizer,
				    &ctx->compress))
		return;

	compress_detect_react_cycles(ctx->messages);
	compress_react_trace(&ctx->messages, &cr);
	rc = compress_summarize(&ctx->messages,
				ctx->compress.max_history_rounds,
				ctx->compress.summarize,
				ctx->compress.summarize_user_data,
				ctx->session_arena,
				&cr);
	log_info("auto-compress: detected+removed %d, summarized %d "
		 "messages (%d -> %d tokens)",
		 cr.messages_removed, cr.messages_summarized,
		 cr.original_tokens, cr.compressed_tokens);
	free(cr.summary);
	key_info_free(cr.preserved);
	(void)rc;
}

static int react_handle_final_response(struct react_context *ctx,
				       const char *proposed,
				       morph_array_t *messages,
				       react_output_cb cb, void *user_data)
{
	struct react_step *final_step;
	int gr;

	gr = react_handle_guardrail_retry(ctx, proposed, messages, cb,
					  user_data);
	if (gr < 0) {
		react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR, gr,
				 "internal_error");
		return 1;
	}
	if (gr == 1)
		return 0;

	final_step = react_step_create(ctx->turn_arena, REACT_STEP_FINAL,
				       proposed, NULL, NULL, NULL);
	add_step(ctx, final_step);
	free(ctx->final_answer);
	ctx->final_answer = strdup(proposed);
	react_set_result(ctx, REACT_OUTCOME_SUCCESS, 0, NULL);
	if (cb)
		cb(REACT_STEP_FINAL, proposed, user_data);
	react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.final",
			      "end", "final answer", proposed);
	return 1;
}

/*
 * Execute the ReAct (Reasoning + Acting) loop.
 *
 * Sends the user input through the LLM, processes tool calls or
 * final answers, and iterates until a final answer is produced,
 * the loop is cancelled, or the maximum iteration count is reached.
 *
 * ctx - ReAct context (must be initialised)
 * user_input - User prompt text
 * cb - Optional output callback for step notifications
 * user_data - Opaque pointer forwarded to cb
 *
 * Returns 0 on success, or a stable negative errno/domain error for the
 * terminal outcome.  Inspect ctx->outcome for the user-facing reason.
 */
int react_run(struct react_context *ctx, const char *user_input,
	      react_output_cb cb, void *user_data)
{
	if (!ctx || !user_input)
		return -EINVAL;
	react_reset(ctx);
	arena_reset(ctx->turn_arena);
	morph_cancel_token_reset(&ctx->cancel_token);
	react_set_state(ctx, REACT_STATE_THINKING);
	react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.turn.begin",
			      "begin", "turn started", user_input);

	if (react_check_input_guardrail(ctx, user_input) > 0)
		MORPH_RETURN(react_finish(ctx));

	struct message_list *msg = msg_list_create(ctx->session_arena, "user", user_input,
						  tokenizer_count(ctx->tokenizer, user_input));
	msg_list_append(&ctx->messages, msg);

	struct model *llm = (struct model *)ctx->llm_model;

	if (!llm || !llm->api_key[0]) {
		react_run_without_llm(ctx, user_input, cb, user_data);
		MORPH_RETURN(react_finish(ctx));
	}

	char *system_prompt = build_system_prompt(ctx, ctx->turn_arena);
	if (!system_prompt) {
		log_err("react_run: failed to build system prompt");
		react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR, -ENOMEM,
				  "internal_error");
		MORPH_RETURN(react_finish(ctx));
	}

	struct tool_desc *active_tools = NULL;
	int active_tool_count = 0;
	int has_tools = ctx->tools && ctx->tools->count > 0;
	if (react_prepare_active_tools(ctx, &active_tools,
				       &active_tool_count) < 0) {
		react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
				 -ENOMEM, "internal_error");
		MORPH_RETURN(react_finish(ctx));
	}

	morph_array_t messages;
	int messages_ready = 0;

	memset(&messages, 0, sizeof(messages));
	if (react_prepare_messages(ctx, &messages) < 0) {
		morph_array_cleanup(&messages);
		react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR, -ENOMEM,
				  "internal_error");
		MORPH_RETURN(react_finish(ctx));
	}
	messages_ready = 1;

	react_active_push(ctx);
	http_set_cancel_flag(&ctx->cancelled);
	http_set_cancel_token(&ctx->cancel_token);

	for (int iteration = 0; iteration < ctx->max_iterations; iteration++) {
		if (react_poll_cancel(ctx, iteration))
			break;

		react_set_state(ctx, REACT_STATE_THINKING);
		react_maybe_compress_context(ctx);

		if (cb)
			cb(REACT_STEP_THOUGHT, "", user_data);

		struct chat_response response = {0};
		time_t llm_start = 0;
		time_t llm_end = 0;
		int status;

		status = react_chat_once(ctx, llm, system_prompt,
					 (struct chat_message *)messages.elts,
					 (int)messages.nelts, active_tools,
					 active_tool_count, cb, user_data,
					 &response, &llm_start, &llm_end);
		if (ctx->outcome != REACT_OUTCOME_NONE) {
			chat_response_free(&response);
			break;
		}

		if (react_sigint_flag) {
			ctx->cancelled = 1;
			morph_cancel_token_cancel(&ctx->cancel_token);
			react_sigint_flag = 0;
		}
		if (react_handle_llm_cancelled(ctx, &response) ||
		    react_handle_llm_error(ctx, &response, status, cb,
					   user_data) ||
		    react_handle_llm_timeout(ctx, &response, llm_start,
					     llm_end, cb, user_data)) {
			chat_response_free(&response);
			break;
		}

		if (response.content && *response.content) {
			struct react_step *thought = react_step_create(ctx->turn_arena,
				REACT_STEP_THOUGHT, response.content, NULL, NULL, NULL);
			add_step(ctx, thought);
			react_emit_text_event(ctx, MORPH_EVENT_REACT,
					      "react.thought.end", "end",
					      NULL, response.content);
		}

		if (response.tool_call_count > 0 && has_tools) {
			int num_tools = response.tool_call_count;
			morph_array_t tool_slots;
			struct react_tool_slot *slots;

			if (react_append_assistant_tool_call_message(
				ctx, &messages, &response) < 0) {
				chat_response_free(&response);
				react_set_result(ctx,
						  REACT_OUTCOME_INTERNAL_ERROR,
						  -ENOMEM, "internal_error");
				break;
			}
			react_set_state(ctx, REACT_STATE_ACTING);
			if (morph_array_init_arena(&tool_slots, ctx->turn_arena,
						   (size_t)num_tools,
						   sizeof(*slots)) < 0) {
				chat_response_free(&response);
				react_set_result(ctx,
						  REACT_OUTCOME_INTERNAL_ERROR,
						  -ENOMEM, "internal_error");
				break;
			}
			slots = morph_array_push_n(&tool_slots,
						   (size_t)num_tools);
			if (!slots) {
				chat_response_free(&response);
				react_set_result(ctx,
						  REACT_OUTCOME_INTERNAL_ERROR,
						  -ENOMEM, "internal_error");
				break;
			}
			memset(slots, 0, (size_t)num_tools * sizeof(*slots));

			react_start_tool_calls(ctx, &response, slots, cb,
					       user_data);
			if (react_join_tool_calls(ctx, &response, slots,
						  &messages, cb, user_data)) {
				chat_response_free(&response);
				break;
			}
		} else {
			const char *proposed = response.content
					       ? response.content : "(no response)";

			if (!react_handle_final_response(ctx, proposed, &messages,
							 cb, user_data)) {
				chat_response_free(&response);
				continue;
			}
			chat_response_free(&response);
			break;
		}

		chat_response_free(&response);
	}

	http_set_cancel_flag(NULL);
	http_set_cancel_token(NULL);
	react_active_pop(ctx);

	react_complete_max_iterations(ctx);
	react_append_final_message(ctx);
	if (messages_ready)
		morph_array_cleanup(&messages);

	MORPH_RETURN(react_finish(ctx));
}
