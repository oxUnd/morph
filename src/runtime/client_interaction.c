#include "runtime/client_internal.h"

#include <nb/cancel.h>
#include <nb/cleanup.h>
#include <nb/time.h>
#include <nb/str.h>
#include <nb/utf8.h>
#include <stdlib.h>
#include <string.h>

static void interaction_destroy(struct runtime_client_interaction *interaction)
{
	if (!interaction)
		return;

	free(interaction->id);
	free(interaction->turn_id);
	free(interaction->kind);
	free(interaction->user_id);
	cJSON_Delete(interaction->request);
	cJSON_Delete(interaction->reply);
	free(interaction);
}

static int interaction_deliver_locked(struct runtime_client_interaction *interaction)
{
	cJSON *data NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *request = cJSON_Duplicate(interaction->request, 1);
	if (!data || !request) {
		cJSON_Delete(request);
		return NB_ERR_NOMEM;
	}

	cJSON_AddStringToObject(data, "kind", interaction->kind);
	cJSON_AddStringToObject(data, "interaction_id", interaction->id);
	cJSON_AddNumberToObject(data, "session_id", (double)interaction->lane->session.id);
	cJSON_AddStringToObject(data, "turn_id", interaction->turn_id);
	cJSON_AddItemToObject(data, "request", request);
	struct runtime_message *message NB_CLEANUP(runtime_message_cleanup) =
		runtime_client_message_create(RUNTIME_MESSAGE_SERVER_REQUEST, interaction->id,
			"interaction.request", interaction->lane->session.id, interaction->turn_id,
			0, data, 0);
	if (!message)
		return NB_ERR_NOMEM;

	return runtime_client_enqueue_locked(interaction->controller, &message);
}

static void interaction_resolved_locked(struct runtime_client_interaction *interaction,
	const char *reason)
{
	cJSON *event NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *data = cJSON_CreateObject();
	if (!event || !data) {
		cJSON_Delete(data);
		return;
	}

	cJSON_AddStringToObject(event, "type", "interaction");
	cJSON_AddStringToObject(event, "name", "interaction.resolved");
	cJSON_AddStringToObject(event, "turn_id", interaction->turn_id);
	cJSON_AddStringToObject(data, "interaction_id", interaction->id);
	cJSON_AddStringToObject(data, "request_id", interaction->id);
	cJSON_AddStringToObject(data, "reason", reason);
	cJSON_AddItemToObject(event, "data", data);
	runtime_client_lane_publish_locked(interaction->lane, "interaction.resolved", event);
}

void runtime_client_interactions_cancel_locked(struct runtime_client_lane *lane)
{
	nb_queue *entry;
	NB_QUEUE_FOREACH(entry, &lane->interactions) {
		struct runtime_client_interaction *interaction =
			NB_QUEUE_DATA(entry, struct runtime_client_interaction, node);

		if (!interaction->resolved) {
			interaction->resolved = 1;
			interaction->error_code = NB_ERR_CANCELLED;
			interaction_resolved_locked(interaction, "cancelled");
		}
	}

	nb_cond_broadcast(lane->service->changed);
}

int runtime_client_interaction_request(struct runtime_client_lane *lane,
	const char *kind, const cJSON *request, const struct nb_cancel *cancel,
	int timeout_seconds, cJSON **out)
{
	struct runtime_client_service *service = lane->service;
	struct runtime_client_interaction *interaction = calloc(1, sizeof(*interaction));
	char id[96];

	if (!interaction)
		return NB_ERR_NOMEM;

	interaction->lane = lane;
	interaction->kind = strdup(kind);
	interaction->request = cJSON_Duplicate(request, 1);
	nb_mutex_lock(service->lock);
	int turn_active = lane->turn && lane->turn->active;
	int releasing = !strcmp(kind, "terminal.release");
	if ((!lane->turn && !lane->management_controller && !lane->interaction_controller) ||
	    (turn_active && lane->turn->cancelled && !releasing) || service->stopping) {
		nb_mutex_unlock(service->lock);
		interaction_destroy(interaction);
		return NB_ERR_CANCELLED;
	}

	interaction->controller = turn_active ? lane->turn->controller :
		lane->management_controller ? lane->management_controller :
		lane->interaction_controller;
	unsigned capabilities = interaction->controller ?
		interaction->controller->options.capabilities :
		lane->turn ? lane->turn->controller_capabilities : 0;
	if (!(capabilities & RUNTIME_CLIENT_INTERACT)) {
		nb_mutex_unlock(service->lock);
		interaction_destroy(interaction);
		return NB_ERR_NOT_PERMITTED;
	}

	if (!strncmp(kind, "terminal.", 9) && interaction->controller &&
	    !interaction->controller->options.terminal_handoff) {
		nb_mutex_unlock(service->lock);
		interaction_destroy(interaction);
		return NB_ERR_UNSUPPORTED;
	}

	snprintf(id, sizeof(id), "interaction_%s_%llu", service->epoch,
		(unsigned long long)++service->next_id);
	interaction->id = strdup(id);
	interaction->turn_id = strdup(turn_active ? lane->turn->id : "");
	interaction->user_id = strdup(interaction->controller ?
		interaction->controller->user_id : lane->turn->user_id);
	if (!interaction->kind || !interaction->request || !interaction->id ||
	    !interaction->turn_id || !interaction->user_id) {
		nb_mutex_unlock(service->lock);
		interaction_destroy(interaction);
		return NB_ERR_NOMEM;
	}

	nb_queue_insert_tail(&lane->interactions, &interaction->node);
	int rc = interaction->controller ? interaction_deliver_locked(interaction) : 0;
	int64_t deadline = timeout_seconds > 0 ? nb_time_monotonic_ns() +
		(int64_t)timeout_seconds * INT64_C(1000000000) : 0;
	while (rc == 0 && !interaction->resolved) {
		if (service->stopping || (turn_active && lane->turn->cancelled && !releasing) ||
		    nb_cancel_requested(cancel)) {
			rc = NB_ERR_CANCELLED;
			break;
		}

		if (!interaction->deadline_disabled && deadline > 0 &&
		    nb_time_monotonic_ns() >= deadline) {
			rc = NB_ERR_TIMEOUT;
			break;
		}

		(void)nb_cond_timedwait(service->changed, service->lock, INT64_C(100000000));
	}

	if (!interaction->resolved) {
		interaction->resolved = 1;
		interaction->error_code = rc;
		interaction_resolved_locked(interaction,
			rc == NB_ERR_TIMEOUT ? "timeout" : "cancelled");
	}

	if (rc == 0)
		rc = interaction->error_code;

	if (rc == 0) {
		*out = interaction->reply;
		interaction->reply = NULL;
	}

	nb_queue_remove(&interaction->node);
	nb_mutex_unlock(service->lock);
	interaction_destroy(interaction);
	return rc;
}

static int interaction_reply_valid(const struct runtime_client_interaction *interaction,
	const cJSON *reply)
{
	if (!cJSON_IsObject(reply))
		return 0;

	if (!strncmp(interaction->kind, "terminal.", 9))
		return cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(reply, "ok"));

	if (!strcmp(interaction->kind, "ask_user")) {
		const cJSON *answers = cJSON_GetObjectItemCaseSensitive(reply, "answers");
		const cJSON *minimum =
			cJSON_GetObjectItemCaseSensitive(interaction->request, "min_choices");
		const cJSON *maximum =
			cJSON_GetObjectItemCaseSensitive(interaction->request, "max_choices");
		int count = cJSON_GetArraySize(answers);
		if (!cJSON_IsArray(answers) || count < (int)cJSON_GetNumberValue(minimum) ||
		    count > ASK_USER_CHOICES_MAX ||
		    (cJSON_IsNumber(maximum) && cJSON_GetNumberValue(maximum) > 0 &&
		     count > (int)cJSON_GetNumberValue(maximum)))
			return 0;

		const cJSON *answer;
		size_t total_bytes = 0;
		cJSON_ArrayForEach(answer, answers) {
			if (!cJSON_IsString(answer) || !answer->valuestring[0])
				return 0;

			size_t bytes = strlen(answer->valuestring);
			if (bytes > ASK_USER_ANSWER_MAX_BYTES - total_bytes ||
			    nb_utf8_validate(nb_str_from_cstr(answer->valuestring), NULL) !=
				NB_UTF8_OK)
				return 0;

			total_bytes += bytes;
			for (const cJSON *previous = answers->child; previous != answer;
			     previous = previous->next) {
				if (!strcmp(previous->valuestring, answer->valuestring))
					return 0;
			}
		}

		return 1;
	}

	const char *decision = runtime_client_json_string(reply, "decision");
	return !strcmp(decision, "allow") || !strcmp(decision, "session") ||
		!strcmp(decision, "always") || !strcmp(decision, "deny");
}

int runtime_client_interaction_progress(struct runtime_client_request_node *request)
{
	struct runtime_client_service *service = request->client->service;
	const char *id = runtime_client_json_string(request->params, "interaction_id");
	if (!id[0])
		id = runtime_client_json_string(request->params, "request_id");

	int rc = NB_ERR_NOT_FOUND;
	int64_t session_id = 0;
	nb_mutex_lock(service->lock);
	for (size_t i = 0; i < service->lanes.len && rc == NB_ERR_NOT_FOUND; i++) {
		struct runtime_client_lane **lane = nb_vec_at(&service->lanes, i);
		nb_queue *entry;
		NB_QUEUE_FOREACH(entry, &(*lane)->interactions) {
			struct runtime_client_interaction *interaction =
				NB_QUEUE_DATA(entry, struct runtime_client_interaction, node);

			if (strcmp(id, interaction->id))
				continue;

			session_id = (*lane)->session.id;
			if (interaction->controller != request->client)
				rc = NB_ERR_NOT_PERMITTED;
			else if (interaction->resolved)
				rc = NB_ERR_STALE;
			else {
				interaction->deadline_disabled = 1;
				rc = 0;
			}

			break;
		}
	}

	(void)runtime_client_respond_locked(request, session_id, NULL, NULL, rc);
	nb_cond_broadcast(service->changed);
	nb_mutex_unlock(service->lock);
	return rc;
}

int runtime_client_reply_copy(struct runtime_client *client, const struct runtime_reply *reply)
{
	if (!client || !reply || !reply->id || !reply->id[0] || reply->error_code > 0)
		return NB_ERR_INVAL;

	struct runtime_client_service *service = client->service;
	cJSON *copy NB_CLEANUP(cJSON_DeleteOwner) = reply->result ?
		cJSON_Duplicate(reply->result, 1) : NULL;
	if (reply->result && !copy)
		return NB_ERR_NOMEM;

	nb_mutex_lock(service->lock);
	int rc = client->closed || service->stopping ? NB_ERR_CLOSED : NB_ERR_NOT_FOUND;
	if (!(client->options.capabilities & RUNTIME_CLIENT_INTERACT))
		rc = NB_ERR_NOT_PERMITTED;

	if (rc == NB_ERR_NOT_FOUND) {
		for (size_t i = 0; i < service->lanes.len; i++) {
			struct runtime_client_lane **lane = nb_vec_at(&service->lanes, i);
			nb_queue *entry;
			NB_QUEUE_FOREACH(entry, &(*lane)->interactions) {
				struct runtime_client_interaction *interaction =
					NB_QUEUE_DATA(entry,
						struct runtime_client_interaction, node);

				if (strcmp(interaction->id, reply->id))
					continue;

				if (interaction->controller != client)
					rc = NB_ERR_NOT_PERMITTED;
				else if (interaction->resolved)
					rc = NB_ERR_EXISTS;
				else if (!reply->error_code &&
				    !interaction_reply_valid(interaction, copy))
					rc = NB_ERR_INVAL;
				else {
					interaction->reply = copy;
					copy = NULL;
					interaction->error_code = reply->error_code;
					interaction->resolved = 1;
					interaction_resolved_locked(interaction, "answered");
					nb_cond_broadcast(service->changed);
					rc = 0;
				}

				break;
			}

			if (rc != NB_ERR_NOT_FOUND)
				break;
		}
	}

	nb_mutex_unlock(service->lock);
	return rc;
}

int runtime_client_claim_interactions(struct runtime_client_request_node *request,
	struct runtime_client_lane *lane)
{
	struct runtime_client_service *service = lane->service;
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	nb_mutex_lock(service->lock);
	int rc = 0;
	if (!(request->client->options.capabilities & RUNTIME_CLIENT_INTERACT))
		rc = NB_ERR_NOT_PERMITTED;
	else if ((!lane->turn || !lane->turn->active) && !lane->management_controller &&
		 !lane->interaction_controller && nb_queue_empty(&lane->interactions))
		rc = NB_ERR_NOT_FOUND;
	struct runtime_client *controller = lane->turn && lane->turn->active ?
		lane->turn->controller : lane->management_controller ? lane->management_controller :
		lane->interaction_controller;
	const char *owner =
		controller ? controller->user_id : lane->turn ? lane->turn->user_id : "";
	if (!owner[0] && !nb_queue_empty(&lane->interactions)) {
		struct runtime_client_interaction *first = NB_QUEUE_DATA(
			nb_queue_head(&lane->interactions),
			struct runtime_client_interaction, node);

		owner = first->user_id;
	}

	if (rc == 0 && strcmp(owner, request->client->user_id))
		rc = NB_ERR_NOT_PERMITTED;
	else if (rc == 0 && controller && controller != request->client && !controller->closed)
		rc = NB_ERR_BUSY;

	if (rc == 0 && !request->client->options.terminal_handoff) {
		nb_queue *entry;
		NB_QUEUE_FOREACH(entry, &lane->interactions) {
			struct runtime_client_interaction *interaction =
				NB_QUEUE_DATA(entry, struct runtime_client_interaction, node);

			if (!interaction->resolved && !strncmp(interaction->kind, "terminal.", 9)) {
				rc = NB_ERR_UNSUPPORTED;
				break;
			}
		}
	}

	if (rc == 0) {
		if (lane->turn && lane->turn->active)
			lane->turn->controller = request->client;
		else
			lane->management_controller = request->client;

		lane->interaction_controller = request->client;
		(void)runtime_client_subscribe_locked(request->client, lane->session.id);
	}

	(void)runtime_client_respond_locked(request, lane->session.id,
		lane->turn ? lane->turn->id : NULL, &result, rc);
	if (rc == 0) {
		nb_queue *entry;
		NB_QUEUE_FOREACH(entry, &lane->interactions) {
			struct runtime_client_interaction *interaction =
				NB_QUEUE_DATA(entry, struct runtime_client_interaction, node);

			if (!interaction->resolved) {
				interaction->controller = request->client;
				(void)interaction_deliver_locked(interaction);
			}
		}
	}

	nb_mutex_unlock(service->lock);
	return rc;
}

int runtime_client_terminal_acquire(void *data)
{
	struct runtime_client_lane *lane = data;
	cJSON *request NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *reply NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!request)
		return NB_ERR_NOMEM;

	int rc = runtime_client_interaction_request(lane, "terminal.acquire", request,
		NULL, 30, &reply);
	if (rc != 0)
		return rc;

	return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ok")) ?
		0 : NB_ERR_NOT_PERMITTED;
}

void runtime_client_terminal_release(void *data)
{
	struct runtime_client_lane *lane = data;
	cJSON *request NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *reply NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (request)
		(void)runtime_client_interaction_request(lane, "terminal.release", request,
			NULL, 5, &reply);
}

static void approval_decisions(cJSON *request)
{
	cJSON *choices = cJSON_CreateArray();
	if (!choices)
		return;

	cJSON_AddItemToArray(choices, cJSON_CreateString("allow"));
	cJSON_AddItemToArray(choices, cJSON_CreateString("session"));
	cJSON_AddItemToArray(choices, cJSON_CreateString("always"));
	cJSON_AddItemToArray(choices, cJSON_CreateString("deny"));
	cJSON_AddItemToObject(request, "allowed_decisions", choices);
}

enum hitl_verdict runtime_client_hitl(const char *tool, const char *args, void *data)
{
	struct runtime_client_lane *lane = data;
	cJSON *request NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *reply NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	if (!request)
		return HITL_DENY;

	cJSON_AddStringToObject(request, "tool_name", tool ? tool : "");
	cJSON_AddStringToObject(request, "arguments_json", args ? args : "{}");
	approval_decisions(request);
	if (runtime_client_interaction_request(lane, "tool_approval", request,
		NULL, 0, &reply) != 0)
		return HITL_DENY;

	const char *decision = runtime_client_json_string(reply, "decision");
	return !strcmp(decision, "always") ? HITL_ALWAYS :
		(!strcmp(decision, "allow") || !strcmp(decision, "session")) ?
		HITL_APPROVE : HITL_DENY;
}

enum tool_operation_verdict runtime_client_approval(const struct tool_operation *operation,
	void *data)
{
	struct runtime_client_lane *lane = data;
	cJSON *request NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *reply NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	if (!request)
		return TOOL_OP_DENY;

	cJSON_AddNumberToObject(request, "kind", operation->kind);
	cJSON_AddStringToObject(request, "tool_name",
		operation->tool_name ? operation->tool_name : "");
	cJSON_AddStringToObject(request, "principal",
		operation->principal ? operation->principal : "");
	cJSON_AddStringToObject(request, "action", operation->action ? operation->action : "");
	cJSON_AddStringToObject(request, "target", operation->target ? operation->target : "");
	cJSON_AddStringToObject(request, "scope", operation->scope ? operation->scope : "");
	cJSON_AddStringToObject(request, "details_json",
		operation->details_json ? operation->details_json : "{}");
	cJSON_AddStringToObject(request, "reason", operation->reason ? operation->reason : "");
	cJSON_AddBoolToObject(request, "host", operation->host);
	cJSON_AddBoolToObject(request, "network", operation->network);
	cJSON_AddNumberToObject(request, "path_access", operation->path_access);
	approval_decisions(request);
	if (runtime_client_interaction_request(lane, "operation_approval", request,
		NULL, 0, &reply) != 0)
		return TOOL_OP_DENY;

	const char *decision = runtime_client_json_string(reply, "decision");
	return !strcmp(decision, "always") ? TOOL_OP_ALWAYS :
		!strcmp(decision, "session") ? TOOL_OP_SESSION :
		!strcmp(decision, "allow") ? TOOL_OP_ALLOW : TOOL_OP_DENY;
}

int runtime_client_question(const char *question, const char *const *choices,
	int choices_count, const char *selection_mode, int min_choices, int max_choices,
	const struct ask_user_options *options, char ***answers, int *answers_count, void *data)
{
	struct runtime_client_lane *lane = data;
	cJSON *request NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *reply NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *choice_array = cJSON_CreateArray();
	if (!request || !choice_array) {
		cJSON_Delete(choice_array);
		return NB_ERR_NOMEM;
	}

	cJSON_AddStringToObject(request, "question", question ? question : "");
	cJSON_AddStringToObject(request, "selection_mode",
		selection_mode ? selection_mode : "single");
	cJSON_AddNumberToObject(request, "min_choices", min_choices);
	cJSON_AddNumberToObject(request, "max_choices", max_choices);
	cJSON_AddItemToObject(request, "choices", choice_array);
	for (int i = 0; i < choices_count; i++)
		cJSON_AddItemToArray(choice_array, cJSON_CreateString(choices[i]));

	if (options)
		(void)ask_user_options_to_json(request, options);

	int rc = runtime_client_interaction_request(lane, "ask_user", request,
		options ? options->cancel_token : NULL,
		options && options->optional ? options->timeout_seconds : 0, &reply);
	if (rc != 0)
		return rc;

	const cJSON *array = cJSON_GetObjectItemCaseSensitive(reply, "answers");
	int count = cJSON_GetArraySize(array);
	char **copied = count > 0 ? calloc((size_t)count, sizeof(*copied)) : NULL;
	if (count > 0 && !copied)
		return NB_ERR_NOMEM;

	for (int i = 0; i < count; i++) {
		copied[i] = strdup(cJSON_GetStringValue(cJSON_GetArrayItem(array, i)));
		if (!copied[i]) {
			for (int j = 0; j < i; j++)
				free(copied[j]);

			free(copied);
			return NB_ERR_NOMEM;
		}
	}

	*answers = copied;
	*answers_count = count;
	return 0;
}
