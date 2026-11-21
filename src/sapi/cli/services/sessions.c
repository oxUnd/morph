#include "sapi/cli/services/sessions.h"

#include "sapi/cli/application.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/adapters/runtime/events.h"
#include "sapi/cli/adapters/runtime/observer.h"
#include "sapi/cli/adapters/input/state.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/pages/approval.h"
#include "sapi/cli/pages/question.h"
#include "sapi/cli/pages/subagents.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/models/composer.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/services/session_restore.h"
#include "sapi/cli/services/turn.h"
#include "runtime/client.h"
#include "runtime/runtime.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/time.h>
#include <nb/vec.h>
#include <nb/cancel.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLI_SESSIONS_PUMP_BUDGET 128

struct cli_sessions_prompt {
	char *display;
	char request_id[64];
};

struct cli_sessions_state {
	int64_t id;
	char turn_id[128];
	char request_id[64];
	int active;
	int presentation_active;
	int visited;
	int auto_named;
	int tool_details;
	int finish_owned;
	int awaiting_input;
	int deleted;
	uint64_t sequence;
	char runtime_epoch[64];
	int done;
	int result;
	struct cli_presenter_state *presenter;
	struct cli_transcript_model *transcript;
	struct cli_subagents_session *subagents;
	struct cli_agents_model agents;
	struct cli_composer_model composer;
	struct cli_recap_state recap;
	char image_path[PATH_MAX];
	struct cli_chat_session_state *view;
	nb_vec pending;
	nb_vec prompts;
	int64_t prompts_revision;
};

struct cli_sessions {
	struct runtime_client *client;
	nb_vec states;
	nb_vec waiters;
	int64_t selected;
	uint64_t request_sequence;
	int draining;
	int interaction_active;
	int request_depth;
	nb_mutex *completion_lock;
	nb_cond *completion;
	int stopping;
	int resync_pending;
	struct cli_sessions_interaction_job *interaction_job;
};

struct cli_sessions_interaction_job {
	struct cli_application *application;
	struct runtime_message *message;
	nb_thread *thread;
	nb_atomic_int done;
	struct nb_cancel cancel;
	int result;
};

struct cli_sessions_execute_input {
	const char *input;
	const char *stored_input;
	struct cli_sessions_state *state;
};

struct cli_sessions_command_call {
	const char *method;
	const cJSON *params;
	cJSON **out;
	int current;
	struct cli_sessions_waiter *waiter;
};

struct cli_sessions_waiter {
	char id[64];
	cJSON *data;
	int done;
	int result;
};

static void cli_sessions_waiter_cleanup(struct cli_sessions_waiter *waiter)
{
	cJSON_Delete(waiter->data);
}

struct cli_sessions_create_call {
	const char *name;
	int auto_named;
	struct session *out;
};

struct cli_sessions_input_cancel_scope {
	struct cli_input_state *input;
	const struct nb_cancel *previous;
};

static void cli_sessions_input_cancel_restore(struct cli_sessions_input_cancel_scope *scope)
{
	if (scope->input)
		scope->input->input_cancel_token = scope->previous;
}

static const char *cli_sessions_text(const cJSON *object, const char *name)
{
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
	return text ? text : "";
}

static int cli_sessions_number(const cJSON *object, const char *name, int fallback)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
	return cJSON_IsNumber(value) ? value->valueint : fallback;
}

static struct cli_sessions_state *cli_sessions_find(const struct cli_sessions *sessions,
	int64_t id)
{
	if (!sessions)
		return NULL;

	for (size_t i = 0; i < sessions->states.len; i++) {
		struct cli_sessions_state *state =
			*(struct cli_sessions_state **)nb_vec_at(&sessions->states, i);
		if (!state->deleted && state->id == id)
			return state;
	}

	return NULL;
}

static struct cli_sessions_state *cli_sessions_add(struct cli_sessions *sessions, int64_t id)
{
	struct cli_sessions_state *state = cli_sessions_find(sessions, id);
	if (state)
		return state;

	state = calloc(1, sizeof(*state));
	if (!state)
		return NULL;

	struct cli_sessions_state **slot = nb_vec_push(&sessions->states);
	if (!slot) {
		free(state);
		return NULL;
	}

	struct runtime_message *message = NULL;
	struct cli_sessions_prompt prompt = {0};
	state->id = id;
	nb_vec_init(&state->pending, sizeof(message));
	nb_vec_init(&state->prompts, sizeof(prompt));
	state->prompts_revision = 1;
	*slot = state;
	return state;
}

static void cli_sessions_wakeup(void *opaque)
{
	cli_event_queue_notify(opaque);
}

static void cli_sessions_next_id(struct cli_sessions *sessions, char id[64])
{
	sessions->request_sequence++;
	snprintf(id, 64, "cli-%llu", (unsigned long long)sessions->request_sequence);
}

static enum morph_event_type cli_sessions_event_type(const char *name)
{
	for (int type = MORPH_EVENT_STARTUP; type <= MORPH_EVENT_INTERACTION; type++) {
		if (!strcmp(morph_event_type_name((enum morph_event_type)type), name))
			return (enum morph_event_type)type;
	}

	return MORPH_EVENT_REACT;
}

static void cli_sessions_prompts_clear(struct cli_sessions_state *state)
{
	for (size_t i = 0; i < state->prompts.len; i++)
		free(((struct cli_sessions_prompt *)nb_vec_at(&state->prompts, i))->display);

	nb_vec_clear(&state->prompts);
	state->prompts_revision++;
}

static int cli_sessions_event(struct cli_application *ctx, const struct runtime_message *message)
{
	const cJSON *root = message->data;
	struct morph_event event = {
		.type = cli_sessions_event_type(cli_sessions_text(root, "type")),
		.name = cli_sessions_text(root, "name"),
		.phase = cli_sessions_text(root, "phase"),
		.message = cli_sessions_text(root, "message"),
		.data = cJSON_GetObjectItemCaseSensitive(root, "data"),
		.turn_id = message->turn_id,
	};
	if (!event.name[0])
		event.name = message->method;

	if (!strcmp(message->method, "cli.content.snapshot")) {
		int rc = cli_chat_react_event(ctx, &event);
		return rc < 0 ? rc : 0;
	}

	if (!strcmp(message->method, "interaction.resolved"))
		return 0;

	struct cli_sessions_state *state = cli_sessions_find(ctx->sessions, message->session_id);
	if (state && !strcmp(message->method, "react.user.steer") && state->prompts.len) {
		free(((struct cli_sessions_prompt *)nb_vec_at(&state->prompts, 0))->display);
		nb_vec_remove(&state->prompts, 0);
		state->prompts_revision++;
	}

	if (!strcmp(message->method, "turn.completed")) {
		if (!state)
			return 0;

		if (state->done && message->turn_id && !strcmp(state->turn_id, message->turn_id))
			return 0;

		state->active = 0;
		cli_sessions_prompts_clear(state);
		const cJSON *result = event.data ? event.data : root;
		int rc = cli_sessions_number(result, "execution_rc", message->error_code);
		if (!rc)
			rc = cli_sessions_number(result, "error_code", 0);

		if (!rc)
			rc = cli_sessions_number(result, "persistence_rc", 0);

		nb_mutex_lock(ctx->sessions->completion_lock);
		state->done = 1;
		state->result = rc;
		nb_cond_broadcast(ctx->sessions->completion);
		nb_mutex_unlock(ctx->sessions->completion_lock);
		if (state->finish_owned)
			cli_turn_finish(ctx, rc);

		return 0;
	}

	return cli_runtime_events_deliver(ctx, &event);
}

static const char *cli_sessions_decision(enum tool_operation_verdict verdict)
{
	switch (verdict) {
	case TOOL_OP_ALLOW:
		return "allow";
	case TOOL_OP_SESSION:
		return "session";
	case TOOL_OP_ALWAYS:
		return "always";
	default:
		return "deny";
	}
}

static int cli_sessions_question(struct cli_application *ctx, const cJSON *request,
	cJSON *result, const struct nb_cancel *cancel)
{
	const cJSON *values = cJSON_GetObjectItemCaseSensitive(request, "choices");
	const char *choices[ASK_USER_CHOICES_MAX] = {0};
	int count = cJSON_GetArraySize(values);
	if (count > ASK_USER_CHOICES_MAX)
		return NB_ERR_TOO_BIG;

	for (int i = 0; i < count; i++)
		choices[i] = cJSON_GetStringValue(cJSON_GetArrayItem(values, i));

	struct ask_user_options options = {
		.optional = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "optional")),
		.timeout_seconds = cli_sessions_number(request, "timeout_seconds", 60),
		.cancel_token = cancel,
	};
	const cJSON *recommended = cJSON_GetObjectItemCaseSensitive(request, "recommended_indices");
	options.recommended_count = cJSON_GetArraySize(recommended);
	if (options.recommended_count > ASK_USER_CHOICES_MAX)
		return NB_ERR_TOO_BIG;

	for (int i = 0; i < options.recommended_count; i++)
		options.recommended_indices[i] = cJSON_GetArrayItem(recommended, i)->valueint;

	const char *policy = cli_sessions_text(request, "response_policy");
	if (!strcmp(policy, "optional"))
		options.optional = 1;

	char **answers = NULL;
	int answers_count = 0;
	int rc = cli_question_callback(cli_sessions_text(request, "question"), choices, count,
		cli_sessions_text(request, "selection_mode"),
		cli_sessions_number(request, "min_choices", 1),
		cli_sessions_number(request, "max_choices", 1), &options,
		&answers, &answers_count, ctx);
	cJSON *output = cJSON_AddArrayToObject(result, "answers");
	if (!output && !rc)
		rc = NB_ERR_NOMEM;

	for (int i = 0; i < answers_count; i++) {
		if (output && !cJSON_AddItemToArray(output, cJSON_CreateString(answers[i])) && !rc)
			rc = NB_ERR_NOMEM;

		free(answers[i]);
	}

	free(answers);
	return rc;
}

static int cli_sessions_interaction(struct cli_application *ctx,
	const struct runtime_message *message)
{
	const cJSON *request = cJSON_GetObjectItemCaseSensitive(message->data, "request");
	const char *kind = cli_sessions_text(message->data, "kind");
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!result)
		return NB_ERR_NOMEM;

	int rc = 0;
	struct cli_sessions_input_cancel_scope cancel_scope
		NB_CLEANUP(cli_sessions_input_cancel_restore) = {
			.input = ctx->input,
			.previous = ctx->input ? ctx->input->input_cancel_token : NULL,
		};
	if (ctx->input)
		ctx->input->input_cancel_token = &ctx->sessions->interaction_job->cancel;

	ctx->sessions->interaction_active = 1;
	if (!strcmp(kind, "terminal.acquire")) {
		cli_runtime_observer_pause(ctx);
		if (!cJSON_AddBoolToObject(result, "ok", 1))
			rc = NB_ERR_NOMEM;
	} else if (!strcmp(kind, "terminal.release")) {
		cli_runtime_observer_resume(ctx);
		if (!cJSON_AddBoolToObject(result, "ok", 1))
			rc = NB_ERR_NOMEM;
	} else if (!strcmp(kind, "ask_user")) {
		int optional = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
			request, "optional")) ||
			!strcmp(cli_sessions_text(request, "response_policy"), "optional");
		if (optional) {
			cJSON *progress NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
			if (!progress || !cJSON_AddStringToObject(progress, "interaction_id",
			    message->id))
				rc = NB_ERR_NOMEM;
			else
				rc = cli_sessions_request(ctx, "interaction.progress",
					progress, NULL);
		}

		if (!rc)
			rc = cli_sessions_question(ctx, request, result,
				&ctx->sessions->interaction_job->cancel);
	} else if (!strcmp(kind, "tool_approval")) {
		enum hitl_verdict verdict = cli_approval_tool_callback(
			cli_sessions_text(request, "tool_name"),
			cli_sessions_text(request, "arguments_json"), ctx);
		const char *decision = verdict == HITL_APPROVE ? "allow"
			: verdict == HITL_ALWAYS ? "always" : "deny";
		if (!cJSON_AddStringToObject(result, "decision", decision))
			rc = NB_ERR_NOMEM;
	} else if (!strcmp(kind, "operation_approval")) {
		struct tool_operation operation = {
			.kind = (enum tool_operation_kind)cli_sessions_number(request, "kind", 0),
			.tool_name = cli_sessions_text(request, "tool_name"),
			.principal = cli_sessions_text(request, "principal"),
			.action = cli_sessions_text(request, "action"),
			.target = cli_sessions_text(request, "target"),
			.scope = cli_sessions_text(request, "scope"),
			.details_json = cli_sessions_text(request, "details_json"),
			.reason = cli_sessions_text(request, "reason"),
			.path_access = (unsigned)cli_sessions_number(request, "path_access", 0),
			.host = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "host")),
			.network = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
				request, "network")),
		};
		const char *decision = cli_sessions_decision(
			cli_approval_operation_callback(&operation, ctx));
		if (!cJSON_AddStringToObject(result, "decision", decision))
			rc = NB_ERR_NOMEM;
	} else
		rc = NB_ERR_UNSUPPORTED;

	struct runtime_reply reply = {.id = message->id, .result = result, .error_code = rc};
	int send_rc = runtime_client_reply_copy(ctx->sessions->client, &reply);
	ctx->sessions->interaction_active = 0;
	return send_rc == NB_ERR_NOT_FOUND ? 0 : send_rc;
}

static int cli_sessions_interaction_on_owner(struct cli_application *ctx, void *opaque)
{
	struct cli_sessions_interaction_job *job = opaque;
	(void)cli_sessions_drain(ctx);
	if (nb_cancel_requested(&job->cancel))
		return 0;

	if (job->message->session_id != ctx->sessions->selected) {
		struct cli_sessions_state *state = cli_sessions_find(ctx->sessions,
			job->message->session_id);
		struct runtime_message **slot = state ? nb_vec_push(&state->pending) : NULL;
		if (!slot)
			return NB_ERR_NOMEM;

		*slot = job->message;
		job->message = NULL;
		return 0;
	}

	return cli_sessions_interaction(ctx, job->message);
}

static void *cli_sessions_interaction_run(void *opaque)
{
	struct cli_sessions_interaction_job *job = opaque;
	job->result = cli_event_queue_call_owner(job->application,
		cli_sessions_interaction_on_owner, job);
	nb_atomic_int_store(&job->done, 1);
	cli_event_queue_notify(job->application);
	return NULL;
}

static int cli_sessions_interaction_start_take(struct cli_application *ctx,
	struct runtime_message **owner)
{
	struct cli_sessions_interaction_job *job = calloc(1, sizeof(*job));
	if (!job)
		return NB_ERR_NOMEM;

	job->application = ctx;
	job->message = *owner;
	nb_atomic_int_init(&job->done, 0);
	nb_cancel_init(&job->cancel);
	int rc = nb_thread_create(&job->thread, cli_sessions_interaction_run, job, NULL);
	if (rc) {
		free(job);
		return rc;
	}

	*owner = NULL;
	ctx->sessions->interaction_job = job;
	return 0;
}

static void cli_sessions_interaction_join(struct cli_sessions *sessions)
{
	struct cli_sessions_interaction_job *job = sessions->interaction_job;
	if (!job)
		return;

	(void)nb_thread_join_take(&job->thread, NULL);
	runtime_message_destroy(job->message);
	free(job);
	sessions->interaction_job = NULL;
}

static int cli_sessions_message(struct cli_application *ctx, struct runtime_message **owner)
{
	struct runtime_message *message = *owner;
	struct cli_sessions *sessions = ctx->sessions;
	if (message->kind == RUNTIME_MESSAGE_EVENT && message->method &&
	    !strcmp(message->method, "runtime.resync_required")) {
		sessions->resync_pending = 1;
		return 0;
	}

	struct cli_sessions_state *state = cli_sessions_find(sessions, message->session_id);
	if (message->kind == RUNTIME_MESSAGE_RESPONSE) {
		int matched = 0;
		nb_mutex_lock(sessions->completion_lock);
		for (size_t i = 0; i < sessions->waiters.len; i++) {
			struct cli_sessions_waiter *waiter =
				*(struct cli_sessions_waiter **)nb_vec_at(&sessions->waiters, i);
			if (strcmp(waiter->id, message->id))
				continue;

			if (!waiter->done) {
				waiter->data = message->data;
				message->data = NULL;
				waiter->result = message->error_code;
				waiter->done = 1;
				nb_cond_broadcast(sessions->completion);
			}

			matched = 1;
			break;
		}

		nb_mutex_unlock(sessions->completion_lock);
		if (matched)
			return 0;

		if (!state) {
			for (size_t i = 0; i < sessions->states.len; i++) {
				struct cli_sessions_state *candidate =
					*(struct cli_sessions_state **)nb_vec_at(
						&sessions->states, i);
				if (!strcmp(message->id, candidate->request_id)) {
					state = candidate;
					break;
				}
			}
		}

		if (state && message->error_code) {
			for (size_t i = 0; i < state->prompts.len; i++) {
				struct cli_sessions_prompt *prompt = nb_vec_at(&state->prompts, i);
				if (strcmp(prompt->request_id, message->id))
					continue;

				free(prompt->display);
				nb_vec_remove(&state->prompts, i);
				state->prompts_revision++;
				if (state->id == sessions->selected)
					CMD_ERROR(ctx, "Could not queue input: %s", nb_err_str(
						morph_error_descriptions(), MORPH_ERROR_COUNT,
						message->error_code));

				break;
			}
		}

		if (state && !strcmp(message->id, state->request_id)) {
			state->request_id[0] = '\0';
			if (message->error_code) {
				state->active = 0;
				nb_mutex_lock(sessions->completion_lock);
				state->done = 1;
				state->result = message->error_code;
				nb_cond_broadcast(sessions->completion);
				nb_mutex_unlock(sessions->completion_lock);
				if (state->id == sessions->selected && state->finish_owned)
					cli_turn_finish(ctx, message->error_code);
			} else {
				snprintf(state->turn_id, sizeof(state->turn_id), "%s",
					cli_sessions_text(message->data, "turn_id"));
			}
		}

		return 0;
	}

	if (message->session_id > 0 && !state)
		state = cli_sessions_add(sessions, message->session_id);

	if (message->session_id > 0 && !state)
		return NB_ERR_NOMEM;

	if (state && message->kind == RUNTIME_MESSAGE_EVENT && message->sequence) {
		if (message->sequence <= state->sequence)
			return 0;

		state->sequence = message->sequence;
		const char *epoch = cli_sessions_text(message->data, "runtime_epoch");
		if (epoch[0])
			snprintf(state->runtime_epoch, sizeof(state->runtime_epoch), "%s", epoch);
	}

	if (state && message->method && !strcmp(message->method, "interaction.resolved")) {
		state->awaiting_input = 0;
		const cJSON *data = cJSON_GetObjectItemCaseSensitive(message->data, "data");
		const char *resolved = cli_sessions_text(data, "interaction_id");
		if (sessions->interaction_job && sessions->interaction_job->message &&
		    !strcmp(sessions->interaction_job->message->id, resolved))
			nb_cancel_request(&sessions->interaction_job->cancel);

		for (size_t i = 0; i < state->pending.len;) {
			struct runtime_message *pending =
				*(struct runtime_message **)nb_vec_at(&state->pending, i);
			if (pending->kind == RUNTIME_MESSAGE_SERVER_REQUEST &&
			    !strcmp(pending->id, resolved)) {
				runtime_message_destroy(pending);
				nb_vec_remove(&state->pending, i);
			} else
				i++;
		}
	}

	if (state && message->kind == RUNTIME_MESSAGE_SERVER_REQUEST)
		state->awaiting_input = 1;

	if (state && message->kind == RUNTIME_MESSAGE_EVENT && message->method &&
	    !strcmp(message->method, "turn.completed") && state->id != sessions->selected)
		state->active = 0;

	if (state) {
		struct runtime_message **slot = nb_vec_push(&state->pending);
		if (!slot)
			return NB_ERR_NOMEM;

		*slot = message;
		*owner = NULL;
		return 0;
	}

	if (message->kind == RUNTIME_MESSAGE_SERVER_REQUEST)
		return cli_sessions_interaction_start_take(ctx, owner);

	return cli_sessions_event(ctx, message);
}

static int cli_sessions_request_send_on_owner(struct cli_application *ctx, void *opaque)
{
	const struct cli_sessions_command_call *call = opaque;
	struct cli_sessions *sessions = ctx->sessions;
	cJSON *copy NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	const cJSON *params = call->params;
	if (call->current) {
		copy = params ? cJSON_Duplicate(params, 1) : cJSON_CreateObject();
		if (!copy)
			return NB_ERR_NOMEM;

		cJSON_DeleteItemFromObjectCaseSensitive(copy, "session_id");
		if (!cJSON_AddNumberToObject(copy, "session_id", (double)sessions->selected))
			return NB_ERR_NOMEM;

		params = copy;
	}

	cli_sessions_next_id(sessions, call->waiter->id);
	nb_mutex_lock(sessions->completion_lock);
	struct cli_sessions_waiter **slot = nb_vec_push(&sessions->waiters);
	if (slot)
		*slot = call->waiter;

	nb_mutex_unlock(sessions->completion_lock);
	if (!slot)
		return NB_ERR_NOMEM;

	struct runtime_command command = {
		.id = call->waiter->id, .method = call->method, .params = params,
	};
	int rc = runtime_client_request_copy(sessions->client, &command);
	if (rc) {
		nb_mutex_lock(sessions->completion_lock);
		nb_vec_pop(&sessions->waiters);
		nb_mutex_unlock(sessions->completion_lock);
	}

	return rc;
}

static int cli_sessions_request_wait(struct cli_application *ctx,
	struct cli_sessions_command_call *call)
{
	struct cli_sessions_waiter waiter NB_CLEANUP(cli_sessions_waiter_cleanup) = {0};
	call->waiter = &waiter;
	int rc = cli_event_queue_call_owner(ctx, cli_sessions_request_send_on_owner, call);
	if (rc)
		return rc;

	struct cli_sessions *sessions = ctx->sessions;
	nb_mutex_lock(sessions->completion_lock);
	while (!waiter.done && !sessions->stopping)
		nb_cond_wait(sessions->completion, sessions->completion_lock);

	rc = waiter.done ? waiter.result : NB_ERR_CANCELLED;
	for (size_t i = 0; i < sessions->waiters.len; i++) {
		struct cli_sessions_waiter *registered =
			*(struct cli_sessions_waiter **)nb_vec_at(&sessions->waiters, i);
		if (registered == &waiter) {
			nb_vec_remove(&sessions->waiters, i);
			break;
		}
	}

	nb_mutex_unlock(sessions->completion_lock);
	if (call->out) {
		*call->out = waiter.data;
		waiter.data = NULL;
	}

	return rc;
}

int cli_sessions_request(struct cli_application *ctx, const char *method,
	const cJSON *params, cJSON **out)
{
	struct cli_sessions *sessions = ctx ? ctx->sessions : NULL;
	if (!sessions || !method || (out && *out))
		return NB_ERR_INVAL;

	if (!cli_event_queue_is_owner(ctx)) {
		struct cli_sessions_command_call call = {
			.method = method, .params = params, .out = out,
		};
		return cli_sessions_request_wait(ctx, &call);
	}

	char id[64];
	cli_sessions_next_id(sessions, id);
	struct runtime_command command = {.id = id, .method = method, .params = params};
	int rc = runtime_client_request_copy(sessions->client, &command);
	if (rc)
		return rc;

	sessions->request_depth++;
	for (;;) {
		struct runtime_message *message NB_CLEANUP(runtime_message_cleanup) = NULL;
		rc = runtime_client_receive(sessions->client, 100, &message);
		if (rc == NB_ERR_AGAIN || rc == NB_ERR_TIMEOUT)
			continue;

		if (rc)
			break;

		if (message->kind == RUNTIME_MESSAGE_RESPONSE && !strcmp(message->id, id)) {
			rc = message->error_code;
			if (out) {
				*out = message->data;
				message->data = NULL;
			}

			break;
		}

		rc = cli_sessions_message(ctx, &message);
		if (rc)
			break;
	}

	sessions->request_depth--;
	return rc;
}

static int cli_sessions_subscribe(struct cli_application *ctx,
	struct cli_sessions_state *state)
{
	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *snapshot NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	if (!params || !cJSON_AddNumberToObject(params, "session_id", (double)state->id))
		return NB_ERR_NOMEM;

	int rc = cli_sessions_request(ctx, "session.subscribe", params, &snapshot);
	if (rc)
		return rc;

	const char *status = cli_sessions_text(snapshot, "status");
	state->active = !strcmp(status, "queued") || !strcmp(status, "preparing") ||
		!strcmp(status, "running") || !strcmp(status, "cancelling");
	state->awaiting_input = cJSON_GetArraySize(
		cJSON_GetObjectItemCaseSensitive(snapshot, "pending_interactions")) > 0;
	snprintf(state->turn_id, sizeof(state->turn_id), "%s",
		cli_sessions_text(snapshot, "turn_id"));
	return 0;
}

static uint64_t cli_sessions_sequence(const cJSON *object, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
	return cJSON_IsNumber(value) && value->valuedouble > 0 &&
		value->valuedouble < (double)INT64_MAX ? (uint64_t)value->valuedouble : 0;
}

static int cli_sessions_event_copy(struct cli_application *ctx,
	struct cli_sessions_state *state, const cJSON *event)
{
	struct runtime_message *message NB_CLEANUP(runtime_message_cleanup) =
		calloc(1, sizeof(*message));
	if (!message)
		return NB_ERR_NOMEM;

	message->kind = RUNTIME_MESSAGE_EVENT;
	message->session_id = state->id;
	message->sequence = cli_sessions_sequence(event, "sequence");
	message->method = strdup(cli_sessions_text(event, "name"));
	message->turn_id = strdup(cli_sessions_text(event, "turn_id"));
	message->data = cJSON_Duplicate(event, 1);
	if (!message->method || !message->turn_id || !message->data)
		return NB_ERR_NOMEM;

	return cli_sessions_message(ctx, &message);
}

static int cli_sessions_snapshot_event(struct cli_application *ctx,
	struct cli_sessions_state *state, const char *name, const cJSON *data)
{
	cJSON *event NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *copy NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Duplicate(data, 1);
	if (!event || !copy || !cJSON_AddStringToObject(event, "type", "react") ||
	    !cJSON_AddStringToObject(event, "name", name) ||
	    !cJSON_AddStringToObject(event, "phase", "end") ||
	    !cJSON_AddStringToObject(event, "turn_id", state->turn_id) ||
	    !cJSON_AddItemToObject(event, "data", copy))
		return NB_ERR_NOMEM;

	copy = NULL;
	return cli_sessions_event_copy(ctx, state, event);
}

static int cli_sessions_completion_pending(const struct cli_sessions_state *state)
{
	for (size_t i = 0; i < state->pending.len; i++) {
		const struct runtime_message *message =
			*(struct runtime_message **)nb_vec_at(&state->pending, i);
		if (message->kind == RUNTIME_MESSAGE_EVENT &&
		    !strcmp(message->method, "turn.completed") &&
		    !strcmp(message->turn_id ? message->turn_id : "", state->turn_id))
			return 1;
	}

	return 0;
}

static int cli_sessions_interaction_pending(const cJSON *interactions, const char *id)
{
	const cJSON *interaction;
	cJSON_ArrayForEach(interaction, interactions) {
		if (!strcmp(cli_sessions_text(interaction, "interaction_id"), id))
			return 1;
	}

	return 0;
}

static void cli_sessions_interactions_resync(struct cli_sessions *sessions,
	struct cli_sessions_state *state, const cJSON *snapshot)
{
	const cJSON *interactions = cJSON_GetObjectItemCaseSensitive(snapshot,
		"pending_interactions");
	for (size_t i = 0; i < state->pending.len;) {
		struct runtime_message *message =
			*(struct runtime_message **)nb_vec_at(&state->pending, i);
		if (message->kind == RUNTIME_MESSAGE_SERVER_REQUEST &&
		    !cli_sessions_interaction_pending(interactions, message->id)) {
			runtime_message_destroy(message);
			nb_vec_remove(&state->pending, i);
		} else
			i++;
	}

	struct cli_sessions_interaction_job *job = sessions->interaction_job;
	if (job && job->message && job->message->session_id == state->id &&
	    !cli_sessions_interaction_pending(interactions, job->message->id))
		nb_cancel_request(&job->cancel);

	state->awaiting_input = cJSON_GetArraySize(interactions) > 0;
}

static int cli_sessions_resync(struct cli_application *ctx)
{
	struct cli_sessions *sessions = ctx->sessions;
	sessions->resync_pending = 0;
	for (size_t i = 0; i < sessions->states.len; i++) {
		struct cli_sessions_state *state =
			*(struct cli_sessions_state **)nb_vec_at(&sessions->states, i);
		if (state->deleted)
			continue;

		cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		cJSON *snapshot NB_CLEANUP(cJSON_DeleteOwner) = NULL;
		if (!params || !cJSON_AddNumberToObject(params, "session_id", (double)state->id) ||
		    !cJSON_AddNumberToObject(params, "since_sequence", (double)state->sequence) ||
		    !cJSON_AddStringToObject(params, "runtime_epoch", state->runtime_epoch))
			return NB_ERR_NOMEM;

		int rc = cli_sessions_request(ctx, "session.subscribe", params, &snapshot);
		if (rc)
			return rc;

		const char *epoch = cli_sessions_text(snapshot, "runtime_epoch");
		if (state->runtime_epoch[0] && strcmp(epoch, state->runtime_epoch))
			state->sequence = 0;

		snprintf(state->runtime_epoch, sizeof(state->runtime_epoch), "%s", epoch);
		const cJSON *events = cJSON_GetObjectItemCaseSensitive(snapshot, "events");
		uint64_t cursor = cli_sessions_sequence(snapshot, "cursor");
		uint64_t first = cli_sessions_sequence(cJSON_GetArrayItem(events, 0), "sequence");
		int gap = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
			snapshot, "resync_required")) ||
			(first > state->sequence + 1) || (!first && cursor > state->sequence);
		const char *status = cli_sessions_text(snapshot, "status");
		int terminal = !strcmp(status, "completed") || !strcmp(status, "interrupted") ||
			!strcmp(status, "failed");
		int recover_completion = terminal && gap && !state->done &&
			!cli_sessions_completion_pending(state);
		int recover_preview = !terminal && gap && state->active && cli_chat_active(ctx);
		const cJSON *event;
		cJSON_ArrayForEach(event, events) {
			const char *name = cli_sessions_text(event, "name");
			if ((recover_preview && (!strcmp(name, "react.content.delta") ||
			    !strcmp(name, "react.final.delta"))) || (recover_completion &&
			    (!strncmp(name, "react.content.", 14) || !strcmp(name, "react.final") ||
			    !strcmp(name, "react.final.delta") || !strcmp(name, "turn.completed"))))
				continue;

			rc = cli_sessions_event_copy(ctx, state, event);
			if (rc)
				return rc;
		}

		cli_sessions_interactions_resync(sessions, state, snapshot);
		if (recover_preview && cli_sessions_text(snapshot, "stream_text")[0]) {
			cJSON *preview NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
			if (!preview || !cJSON_AddStringToObject(preview, "text",
			    cli_sessions_text(snapshot, "stream_text")))
				return NB_ERR_NOMEM;

			rc = cli_sessions_snapshot_event(ctx, state, "cli.content.snapshot",
				preview);
			if (rc)
				return rc;
		}

		if (recover_completion) {
			snprintf(state->turn_id, sizeof(state->turn_id), "%s",
				cli_sessions_text(snapshot, "turn_id"));
			const char *final = cli_sessions_text(snapshot, "final_text");
			if (final[0]) {
				cJSON *answer NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
				if (!answer || !cJSON_AddStringToObject(answer, "text", final))
					return NB_ERR_NOMEM;

				rc = cli_sessions_snapshot_event(ctx, state, "react.final", answer);
				if (rc)
					return rc;
			}

			rc = cli_sessions_snapshot_event(ctx, state, "turn.completed", snapshot);
			if (rc)
				return rc;
		}

		if (cursor > state->sequence)
			state->sequence = cursor;
	}

	return 0;
}

int cli_sessions_init(struct cli_application *ctx)
{
	if (!ctx || !ctx->runtime || ctx->sessions)
		return NB_ERR_INVAL;

	struct cli_sessions *sessions = calloc(1, sizeof(*sessions));
	if (!sessions)
		return NB_ERR_NOMEM;

	struct cli_sessions_state *state = NULL;
	nb_vec_init(&sessions->states, sizeof(state));
	struct cli_sessions_waiter *waiter = NULL;
	nb_vec_init(&sessions->waiters, sizeof(waiter));
	sessions->completion_lock = nb_mutex_new(0);
	sessions->completion = nb_cond_new();
	if (!sessions->completion_lock || !sessions->completion) {
		nb_cond_free(sessions->completion);
		nb_mutex_free(sessions->completion_lock);
		nb_vec_cleanup(&sessions->states);
		nb_vec_cleanup(&sessions->waiters);
		free(sessions);
		return NB_ERR_NOMEM;
	}

	struct runtime_client_options options = {
		.wakeup = cli_sessions_wakeup,
		.wakeup_user_data = ctx,
		.terminal_handoff = 1,
	};
	int rc = runtime_client_open(ctx->runtime, &options, &sessions->client);
	if (rc) {
		nb_cond_free(sessions->completion);
		nb_mutex_free(sessions->completion_lock);
		nb_vec_cleanup(&sessions->states);
		nb_vec_cleanup(&sessions->waiters);
		free(sessions);
		return rc;
	}

	ctx->sessions = sessions;
	struct session current = {0};
	if (!runtime_session_current(ctx->runtime, &current) && current.id > 0) {
		state = cli_sessions_add(sessions, current.id);
		if (!state) {
			cli_sessions_cleanup(ctx);
			return NB_ERR_NOMEM;
		}

		state->visited = 1;
		state->auto_named = ctx->session_auto_named;
		sessions->selected = current.id;
		rc = cli_sessions_subscribe(ctx, state);
		if (rc) {
			cli_sessions_cleanup(ctx);
			return rc;
		}
	}

	return 0;
}

static void cli_sessions_projection_save(struct cli_application *ctx,
	struct cli_sessions_state *state)
{
	state->presenter = ctx->presenter;
	ctx->presenter = NULL;
	state->transcript = ctx->transcript;
	ctx->transcript = NULL;
	state->subagents = ctx->subagents;
	ctx->subagents = NULL;
	state->agents = ctx->agents;
	memset(&ctx->agents, 0, sizeof(ctx->agents));
	state->composer = ctx->composer;
	memset(&ctx->composer, 0, sizeof(ctx->composer));
	state->recap = ctx->recap;
	memset(&ctx->recap, 0, sizeof(ctx->recap));
	ctx->recap.renderer = state->recap.renderer;
	memcpy(state->image_path, ctx->image_path, sizeof(state->image_path));
	ctx->image_path[0] = '\0';
	state->auto_named = ctx->session_auto_named;
	state->tool_details = ctx->tool_details;
	state->presentation_active = ctx->turn_active;
}

static void cli_sessions_projection_restore(struct cli_application *ctx,
	struct cli_sessions_state *state)
{
	ctx->presenter = state->presenter;
	state->presenter = NULL;
	ctx->transcript = state->transcript;
	state->transcript = NULL;
	ctx->subagents = state->subagents;
	state->subagents = NULL;
	ctx->agents = state->agents;
	memset(&state->agents, 0, sizeof(state->agents));
	ctx->composer = state->composer;
	memset(&state->composer, 0, sizeof(state->composer));
	ctx->recap = state->recap;
	memset(&state->recap, 0, sizeof(state->recap));
	memcpy(ctx->image_path, state->image_path, sizeof(ctx->image_path));
	state->image_path[0] = '\0';
	ctx->session_auto_named = state->auto_named;
	ctx->tool_details = state->tool_details;
	ctx->turn_active = state->presentation_active;
	ctx->input_status.updated = 0;
}

static int cli_sessions_select_on_owner(struct cli_application *ctx, void *opaque)
{
	return cli_sessions_select(ctx, *(const int64_t *)opaque);
}

int cli_sessions_select(struct cli_application *ctx, int64_t id)
{
	if (!ctx || !ctx->sessions)
		return NB_ERR_INVAL;

	if (!cli_event_queue_is_owner(ctx))
		return cli_event_queue_call_owner(ctx, cli_sessions_select_on_owner, &id);

	struct cli_sessions *sessions = ctx->sessions;
	if (sessions->selected == id)
		return 0;

	struct session selected = {0};
	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!params || !cJSON_AddNumberToObject(params, "session_id", (double)id))
		return NB_ERR_NOMEM;

	int rc = cli_sessions_request(ctx, "session.get", params, NULL);
	if (rc)
		return rc;

	struct cli_sessions_state *next = cli_sessions_add(sessions, id);
	if (!next)
		return NB_ERR_NOMEM;

	rc = cli_sessions_subscribe(ctx, next);
	if (rc)
		return rc;

	struct cli_sessions_state *previous = cli_sessions_find(sessions, sessions->selected);
	char *command_input NB_CLEANUP(nb_charp_cleanup) = NULL;
	if (previous) {
		rc = cli_chat_session_command_copy(ctx, &command_input);
		if (rc)
			return rc;

		rc = cli_chat_session_save(ctx, &previous->view);
		if (rc)
			return rc;

		cli_sessions_projection_save(ctx, previous);
	}

	rc = runtime_session_select_existing(ctx->runtime, id, &selected);
	if (rc) {
		if (previous) {
			cli_sessions_projection_restore(ctx, previous);
			(void)cli_chat_session_restore(ctx, &previous->view);
		}

		return rc;
	}

	if (selected.workdir[0])
		snprintf(ctx->workdir, sizeof(ctx->workdir), "%s", selected.workdir);

	sessions->selected = id;
	if (next->visited) {
		cli_sessions_projection_restore(ctx, next);
		rc = cli_chat_session_restore(ctx, &next->view);
		if (!rc)
			rc = cli_chat_session_refresh(ctx);
	} else {
		if (previous) {
			rc = cli_presentation_init(ctx);
			if (!rc)
				rc = cli_composer_model_init(&ctx->composer);
		}

		ctx->turn_active = next->active;
		ctx->session_auto_named = 1;
		if (!rc && previous)
			rc = cli_chat_session_empty(ctx);

		if (!rc)
			rc = cli_chat_session_refresh(ctx);

		if (!rc)
			rc = cli_session_history_restore(ctx);

		next->visited = 1;
	}

	if (!rc)
		rc = cli_sessions_drain(ctx);

	if (!rc && command_input)
		rc = cli_chat_command_input(ctx, command_input);

	return rc;
}

static int cli_sessions_create_on_owner(struct cli_application *ctx, void *opaque)
{
	const struct cli_sessions_create_call *call = opaque;
	return cli_sessions_create(ctx, call->name, call->auto_named, call->out);
}

int cli_sessions_create(struct cli_application *ctx, const char *name,
	int auto_named, struct session *out)
{
	if (!ctx || !ctx->sessions || !name)
		return NB_ERR_INVAL;

	if (!cli_event_queue_is_owner(ctx)) {
		struct cli_sessions_create_call call = {
			.name = name, .auto_named = auto_named, .out = out,
		};
		return cli_event_queue_call_owner(ctx, cli_sessions_create_on_owner, &call);
	}

	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	if (!params || !cJSON_AddStringToObject(params, "name", name) ||
	    !cJSON_AddStringToObject(params, "workdir", ctx->workdir))
		return NB_ERR_NOMEM;

	int rc = cli_sessions_request(ctx, "session.create", params, &result);
	if (rc)
		return rc;

	const cJSON *session = cJSON_GetObjectItemCaseSensitive(result, "session");
	if (!session)
		session = result;

	const cJSON *number = cJSON_GetObjectItemCaseSensitive(session, "id");
	if (!cJSON_IsNumber(number))
		number = cJSON_GetObjectItemCaseSensitive(session, "session_id");

	if (!cJSON_IsNumber(number))
		return NB_ERR_PROTOCOL;

	int64_t id = (int64_t)number->valuedouble;
	rc = cli_sessions_select(ctx, id);
	if (!rc) {
		ctx->session_auto_named = !auto_named;
		if (out)
			rc = runtime_session_current(ctx->runtime, out);
	}

	return rc;
}

static int cli_sessions_delete_on_owner(struct cli_application *ctx, void *opaque)
{
	return cli_sessions_delete(ctx, *(const int64_t *)opaque);
}

int cli_sessions_delete(struct cli_application *ctx, int64_t id)
{
	if (!ctx || !ctx->sessions || id <= 0 || id == ctx->sessions->selected)
		return NB_ERR_INVAL;

	if (!cli_event_queue_is_owner(ctx))
		return cli_event_queue_call_owner(ctx, cli_sessions_delete_on_owner, &id);

	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!params || !cJSON_AddNumberToObject(params, "session_id", (double)id))
		return NB_ERR_NOMEM;

	int rc = cli_sessions_request(ctx, "session.delete", params, NULL);
	struct cli_sessions_state *state = cli_sessions_find(ctx->sessions, id);
	if (!rc && state)
		state->deleted = 1;

	return rc;
}

int cli_sessions_submit(struct cli_application *ctx, const char *input,
	const char *stored_input)
{
	if (!ctx || !ctx->sessions || !input)
		return NB_ERR_INVAL;

	struct cli_sessions *sessions = ctx->sessions;
	if (sessions->stopping)
		return NB_ERR_CLOSED;

	if (sessions->selected <= 0) {
		char name[96];
		snprintf(name, sizeof(name), "new_%lld_%llu", (long long)time(NULL),
			(unsigned long long)++sessions->request_sequence);
		int rc = cli_sessions_create(ctx, name, 1, NULL);
		if (rc)
			return rc;
	}

	struct cli_sessions_state *state = cli_sessions_find(sessions, sessions->selected);
	if (!state)
		return NB_ERR_STATE;

	nb_buf model_input NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *effective_input = input;
	if (ctx->image_path[0]) {
		if (!nb_buf_printf(&model_input, "[Image: %s]\n"
		    "(The user attached the image above; if the task involves "
		    "understanding or reading the image, use img_qa to view it.)\n%s",
		    ctx->image_path, input))
			return nb_buf_status(&model_input);

		effective_input = nb_buf_cstr(&model_input);
	}

	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!params || !cJSON_AddNumberToObject(params, "session_id", (double)state->id) ||
	    !cJSON_AddStringToObject(params, "input", effective_input) ||
	    !cJSON_AddStringToObject(params, "stored_user_input",
		stored_input ? stored_input : input))
		return NB_ERR_NOMEM;

	char id[64];
	cli_sessions_next_id(sessions, id);
	const char *method = state->active ? "turn.steer" : "turn.start";
	if (state->active) {
		if (!state->turn_id[0])
			return NB_ERR_BUSY;

		if (!cJSON_AddStringToObject(params, "turn_id", state->turn_id) ||
		    !cJSON_AddStringToObject(params, "display_text",
			stored_input ? stored_input : input) ||
		    !cJSON_AddBoolToObject(params, "expedite", 0))
			return NB_ERR_NOMEM;
	} else {
		if (!cJSON_AddNumberToObject(params, "turn_flags", AGENT_TURN_DEFAULT_FLAGS |
		    AGENT_TURN_SAVE_EMPTY_USER | AGENT_TURN_SAVE_EMPTY_ASSISTANT) ||
		    !cJSON_AddBoolToObject(params, "generate_session_title",
			!ctx->session_auto_named))
			return NB_ERR_NOMEM;
	}

	char *display NB_CLEANUP(nb_charp_cleanup) = state->active
		? strdup(stored_input ? stored_input : input) : NULL;
	if (state->active && (!display || !nb_vec_reserve(&state->prompts, state->prompts.len + 1)))
		return NB_ERR_NOMEM;

	struct cli_sessions_prompt *queued = state->active ? nb_vec_push(&state->prompts) : NULL;
	if (state->active && !queued)
		return NB_ERR_NOMEM;

	struct runtime_command command = {.id = id, .method = method, .params = params};
	int rc = runtime_client_request_copy(sessions->client, &command);
	if (!rc)
		ctx->image_path[0] = '\0';

	if (!rc && queued) {
		queued->display = nb_charp_steal(&display);
		snprintf(queued->request_id, sizeof(queued->request_id), "%s", id);
		state->prompts_revision++;
	} else if (queued)
		nb_vec_pop(&state->prompts);

	if (!rc && !state->active) {
		snprintf(state->request_id, sizeof(state->request_id), "%s", id);
		state->active = 1;
		state->finish_owned = !ctx->turn_active;
		nb_mutex_lock(sessions->completion_lock);
		state->done = 0;
		state->result = 0;
		nb_mutex_unlock(sessions->completion_lock);
		ctx->session_auto_named = 1;
		if (cli_chat_active(ctx))
			cli_presentation_user_input(ctx, stored_input ? stored_input : input);

		if (state->finish_owned)
			cli_turn_begin(ctx);
	}

	return rc;
}

static int cli_sessions_execute_on_owner(struct cli_application *ctx, void *opaque)
{
	struct cli_sessions_execute_input *input = opaque;
	if (cli_sessions_active(ctx))
		return NB_ERR_BUSY;

	int rc = cli_sessions_submit(ctx, input->input, input->stored_input);
	if (!rc)
		input->state = cli_sessions_find(ctx->sessions, ctx->sessions->selected);

	return rc;
}

int cli_sessions_execute(struct cli_application *ctx, const char *input,
	const char *stored_input)
{
	if (!ctx || !ctx->sessions || !input)
		return NB_ERR_INVAL;

	struct cli_sessions_execute_input request = {
		.input = input,
		.stored_input = stored_input,
	};
	int rc = cli_event_queue_call_owner(ctx, cli_sessions_execute_on_owner, &request);
	if (rc)
		return rc;

	struct cli_sessions *sessions = ctx->sessions;
	nb_mutex_lock(sessions->completion_lock);
	while (!request.state->done && !sessions->stopping)
		nb_cond_wait(sessions->completion, sessions->completion_lock);

	rc = sessions->stopping ? NB_ERR_CANCELLED : request.state->result;
	nb_mutex_unlock(sessions->completion_lock);
	return rc;
}

int cli_sessions_drain(struct cli_application *ctx)
{
	struct cli_sessions *sessions = ctx ? ctx->sessions : NULL;
	if (!sessions || sessions->draining || !cli_event_queue_events_enabled(ctx))
		return 0;

	if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE &&
	    nb_atomic_int_exchange(&ctx->interrupted, 0))
		(void)cli_sessions_interrupt(ctx, 0);

	if (sessions->interaction_job && nb_atomic_int_load(&sessions->interaction_job->done))
		cli_sessions_interaction_join(sessions);

	sessions->draining = 1;
	int rc = 0;
	struct cli_sessions_state *selected = cli_sessions_find(sessions, sessions->selected);
	if (selected && !sessions->interaction_active && !sessions->interaction_job &&
	    !sessions->request_depth) {
		for (int i = 0; i < CLI_SESSIONS_PUMP_BUDGET && selected->pending.len && !rc &&
		     !sessions->interaction_job; i++) {
			struct runtime_message *message NB_CLEANUP(runtime_message_cleanup) =
				*(struct runtime_message **)nb_vec_at(&selected->pending, 0);
			nb_vec_remove(&selected->pending, 0);
			if (message->kind == RUNTIME_MESSAGE_SERVER_REQUEST)
				rc = cli_sessions_interaction_start_take(ctx, &message);
			else
				rc = cli_sessions_event(ctx, message);
		}
	}

	for (int i = 0; i < CLI_SESSIONS_PUMP_BUDGET && !rc; i++) {
		struct runtime_message *message NB_CLEANUP(runtime_message_cleanup) = NULL;
		rc = runtime_client_try_receive(sessions->client, &message);
		if (rc == NB_ERR_AGAIN) {
			rc = 0;
			break;
		}

		if (!rc)
			rc = cli_sessions_message(ctx, &message);
	}

	if (!rc && sessions->resync_pending && !sessions->request_depth)
		rc = cli_sessions_resync(ctx);

	sessions->draining = 0;
	if (!rc && selected && selected->pending.len && !sessions->interaction_active &&
	    !sessions->interaction_job && !sessions->request_depth)
		cli_event_queue_notify(ctx);

	return rc;
}

int cli_sessions_interrupt(struct cli_application *ctx, int preserve_processes)
{
	struct cli_sessions *sessions = ctx ? ctx->sessions : NULL;
	struct cli_sessions_state *state = cli_sessions_find(sessions,
		sessions ? sessions->selected : 0);
	if (!state || !state->active)
		return 0;

	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!params || !cJSON_AddNumberToObject(params, "session_id", (double)state->id) ||
	    !cJSON_AddStringToObject(params, "turn_id", state->turn_id) ||
	    !cJSON_AddBoolToObject(params, "preserve_processes", preserve_processes))
		return NB_ERR_NOMEM;

	char id[64];
	cli_sessions_next_id(sessions, id);
	struct runtime_command command = {.id = id, .method = "turn.interrupt", .params = params};
	return runtime_client_request_copy(sessions->client, &command);
}

int cli_sessions_expedite(struct cli_application *ctx)
{
	struct cli_sessions *sessions = ctx ? ctx->sessions : NULL;
	struct cli_sessions_state *state = cli_sessions_find(sessions,
		sessions ? sessions->selected : 0);
	if (!state || !state->active || !state->prompts.len)
		return 0;

	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!params || !cJSON_AddNumberToObject(params, "session_id", (double)state->id) ||
	    !cJSON_AddStringToObject(params, "turn_id", state->turn_id))
		return NB_ERR_NOMEM;

	char id[64];
	cli_sessions_next_id(sessions, id);
	struct runtime_command command = {.id = id, .method = "turn.expedite", .params = params};
	int rc = runtime_client_request_copy(sessions->client, &command);
	return rc ? rc : 1;
}

int cli_sessions_prompts_copy_since(struct cli_application *ctx, char **items,
	size_t capacity, size_t *total, int64_t *revision)
{
	if (!ctx || !ctx->sessions || !total || !revision || (capacity && !items))
		return NB_ERR_INVAL;

	struct cli_sessions_state *state = cli_sessions_find(ctx->sessions,
		ctx->sessions->selected);
	int64_t current = state ? state->prompts_revision : 0;
	if (*revision == current)
		return NB_ERR_AGAIN;

	*total = state ? state->prompts.len : 0;
	size_t count = *total < capacity ? *total : capacity;
	for (size_t i = 0; i < count; i++) {
		if (items[i])
			return NB_ERR_EXISTS;
	}

	for (size_t i = 0; i < count; i++) {
		const struct cli_sessions_prompt *prompt = nb_vec_at(&state->prompts, i);
		items[i] = strdup(prompt->display);
		if (!items[i]) {
			for (size_t j = 0; j < i; j++) {
				free(items[j]);
				items[j] = NULL;
			}

			*total = 0;
			return NB_ERR_NOMEM;
		}
	}

	*revision = current;
	return 0;
}

int cli_sessions_active(const struct cli_application *ctx)
{
	const struct cli_sessions *sessions = ctx ? ctx->sessions : NULL;
	const struct cli_sessions_state *state = cli_sessions_find(sessions,
		sessions ? sessions->selected : 0);
	return state && state->active;
}

int cli_sessions_running_count(const struct cli_application *ctx)
{
	const struct cli_sessions *sessions = ctx ? ctx->sessions : NULL;
	int count = 0;
	for (size_t i = 0; sessions && i < sessions->states.len; i++) {
		const struct cli_sessions_state *state =
			*(struct cli_sessions_state **)nb_vec_at(&sessions->states, i);
		count += state->active;
	}

	return count;
}

int cli_sessions_open_count(const struct cli_application *ctx)
{
	const struct cli_sessions *sessions = ctx ? ctx->sessions : NULL;
	int count = 0;
	for (size_t i = 0; sessions && i < sessions->states.len; i++) {
		const struct cli_sessions_state *state =
			*(struct cli_sessions_state **)nb_vec_at(&sessions->states, i);
		count += !state->deleted;
	}

	return count;
}

int64_t cli_sessions_current_id(const struct cli_application *ctx)
{
	return ctx && ctx->sessions ? ctx->sessions->selected : 0;
}

int cli_sessions_is_running(const struct cli_application *ctx, int64_t id)
{
	const struct cli_sessions_state *state = cli_sessions_find(ctx ? ctx->sessions : NULL, id);
	return state && state->active;
}

int cli_sessions_needs_input(const struct cli_application *ctx, int64_t id)
{
	const struct cli_sessions_state *state = cli_sessions_find(ctx ? ctx->sessions : NULL, id);
	if (state && state->awaiting_input)
		return 1;

	for (size_t i = 0; state && i < state->pending.len; i++) {
		const struct runtime_message *message =
			*(struct runtime_message **)nb_vec_at(&state->pending, i);
		if (message->kind == RUNTIME_MESSAGE_SERVER_REQUEST)
			return 1;
	}

	return 0;
}

int cli_sessions_cycle(struct cli_application *ctx, int direction)
{
	struct cli_sessions *sessions = ctx ? ctx->sessions : NULL;
	if (!sessions || cli_sessions_open_count(ctx) < 2)
		return 0;

	size_t current = 0;
	for (size_t i = 0; i < sessions->states.len; i++) {
		const struct cli_sessions_state *state =
			*(struct cli_sessions_state **)nb_vec_at(&sessions->states, i);
		if (state->id == sessions->selected)
			current = i;
	}

	for (size_t i = 0; i < sessions->states.len; i++) {
		current = direction < 0
			? (current ? current - 1 : sessions->states.len - 1)
			: (current + 1) % sessions->states.len;
		const struct cli_sessions_state *state =
			*(struct cli_sessions_state **)nb_vec_at(&sessions->states, current);
		if (!state->deleted)
			return cli_sessions_select(ctx, state->id);
	}

	return 0;
}

int cli_sessions_request_current(struct cli_application *ctx, const char *method,
	const cJSON *params, cJSON **out)
{
	if (!ctx || !ctx->sessions || (out && *out))
		return NB_ERR_INVAL;

	if (!cli_event_queue_is_owner(ctx)) {
		struct cli_sessions_command_call call = {
			.method = method, .params = params, .out = out, .current = 1,
		};
		return cli_sessions_request_wait(ctx, &call);
	}

	cJSON *copy NB_CLEANUP(cJSON_DeleteOwner) = params
		? cJSON_Duplicate(params, 1) : cJSON_CreateObject();
	if (!copy)
		return NB_ERR_NOMEM;

	cJSON_DeleteItemFromObjectCaseSensitive(copy, "session_id");
	if (!cJSON_AddNumberToObject(copy, "session_id", (double)ctx->sessions->selected))
		return NB_ERR_NOMEM;

	return cli_sessions_request(ctx, method, copy, out);
}

void cli_sessions_cleanup(struct cli_application *ctx)
{
	struct cli_sessions *sessions = ctx ? ctx->sessions : NULL;
	if (!sessions)
		return;

	(void)cli_event_queue_stop_owner_calls(ctx);
	cli_sessions_interaction_join(sessions);
	runtime_client_close(sessions->client);
	struct cli_sessions_state current = {0};
	cli_sessions_projection_save(ctx, &current);
	for (size_t i = 0; i < sessions->states.len; i++) {
		struct cli_sessions_state *state =
			*(struct cli_sessions_state **)nb_vec_at(&sessions->states, i);
		for (size_t j = 0; j < state->pending.len; j++)
			runtime_message_destroy(*(struct runtime_message **)nb_vec_at(
				&state->pending, j));

		nb_vec_cleanup(&state->pending);
		cli_sessions_prompts_clear(state);
		nb_vec_cleanup(&state->prompts);
		cli_chat_session_state_destroy(state->view);
		if (state->presenter) {
			cli_sessions_projection_restore(ctx, state);
			cli_subagents_cleanup(ctx);
			cli_presentation_cleanup(ctx);
			cli_composer_model_cleanup(&ctx->composer);
		}

		free(state);
	}

	cli_sessions_projection_restore(ctx, &current);
	nb_cond_free(sessions->completion);
	nb_mutex_free(sessions->completion_lock);
	nb_vec_cleanup(&sessions->states);
	nb_vec_cleanup(&sessions->waiters);
	free(sessions);
	ctx->sessions = NULL;
}

void cli_sessions_stop(struct cli_application *ctx)
{
	struct cli_sessions *sessions = ctx ? ctx->sessions : NULL;
	if (!sessions)
		return;

	nb_mutex_lock(sessions->completion_lock);
	sessions->stopping = 1;
	nb_cond_broadcast(sessions->completion);
	nb_mutex_unlock(sessions->completion_lock);
}
