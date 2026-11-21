#include "runtime/client.h"

#include "runtime/client_internal.h"
#include "runtime/protocol_dispatch.h"
#include "runtime/runtime_internal.h"
#include "permission/context.h"
#include "agent/tools/exec_jobs.h"
#include <nb/cleanup.h>
#include <nb/random.h>
#include <nb/time.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void client_destroy(struct runtime_client *client);
static int service_release_ready_locked(struct runtime_client_service *service);
static void service_destroy(struct runtime_client_service *service);

int runtime_client_service_interrupted(void *data)
{
	struct runtime_client_service *service = data;

	nb_mutex_lock(service->lock);
	int stopping = service->stopping;
	nb_mutex_unlock(service->lock);
	return stopping;
}

static void client_release_locked(struct runtime_client *client)
{
	if (client->references > 0)
		client->references--;

	if (!client->references) {
		nb_queue_remove(&client->node);
		client_destroy(client);
	}
}

const char *runtime_client_json_string(const cJSON *object, const char *name)
{
	const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));

	return value ? value : "";
}

int64_t runtime_client_json_id(const cJSON *object, const char *name)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
	double number = cJSON_GetNumberValue(value);

	if (!cJSON_IsNumber(value) || !isfinite(number) || number <= 0 ||
	    number >= 0x1p63 || floor(number) != number)
		return 0;

	return (int64_t)number;
}

int runtime_client_json_bool(const cJSON *object, const char *name)
{
	return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, name));
}

void runtime_message_destroy(struct runtime_message *message)
{
	if (!message)
		return;

	free(message->id);
	free(message->method);
	free(message->turn_id);
	cJSON_Delete(message->data);
	free(message);
}

void runtime_message_cleanup(struct runtime_message **message)
{
	runtime_message_destroy(*message);
	*message = NULL;
}

struct runtime_message *runtime_client_message_create(enum runtime_message_kind kind,
	const char *id, const char *method, int64_t session_id, const char *turn_id,
	uint64_t sequence, const cJSON *data, int error_code)
{
	struct runtime_message *message NB_CLEANUP(runtime_message_cleanup) =
		calloc(1, sizeof(*message));

	if (!message)
		return NULL;

	message->kind = kind;
	message->id = id ? strdup(id) : NULL;
	message->method = method ? strdup(method) : NULL;
	message->session_id = session_id;
	message->turn_id = turn_id ? strdup(turn_id) : NULL;
	message->sequence = sequence;
	message->data = data ? cJSON_Duplicate(data, 1) : cJSON_CreateObject();
	message->error_code = error_code;
	if ((id && !message->id) || (method && !message->method) ||
	    (turn_id && !message->turn_id) || !message->data)
		return NULL;

	struct runtime_message *result = message;
	message = NULL;
	return result;
}

static void message_queue_clear(nb_queue *messages)
{
	while (!nb_queue_empty(messages)) {
		struct runtime_client_message_node *node =
			NB_QUEUE_DATA(nb_queue_head(messages), struct runtime_client_message_node,
				node);

		nb_queue_remove(&node->node);
		runtime_message_destroy(node->message);
		free(node);
	}
}

static void client_signal_locked(struct runtime_client *client)
{
	nb_cond_broadcast(client->service->changed);
	if (!client->closed && client->options.wakeup)
		client->options.wakeup(client->options.wakeup_user_data);
}

static struct runtime_client_subscription *client_subscription_locked(
	struct runtime_client *client, int64_t session_id)
{
	for (size_t i = 0; i < client->subscriptions.len; i++) {
		struct runtime_client_subscription *subscription =
			nb_vec_at(&client->subscriptions, i);

		if (subscription->session_id == session_id)
			return subscription;
	}

	return NULL;
}

int runtime_client_is_subscribed_locked(struct runtime_client *client, int64_t session_id)
{
	struct runtime_client_subscription *subscription =
		client_subscription_locked(client, session_id);

	return !client->closed && subscription && !subscription->suspended;
}

int runtime_client_subscribe_locked(struct runtime_client *client, int64_t session_id)
{
	struct runtime_client_subscription *subscription =
		client_subscription_locked(client, session_id);

	if (!subscription) {
		subscription = nb_vec_push(&client->subscriptions);
		if (!subscription)
			return NB_ERR_NOMEM;

		subscription->session_id = session_id;
	}

	subscription->suspended = 0;
	return 0;
}

static int client_event_overflow_locked(struct runtime_client *client,
	int64_t session_id, uint64_t sequence)
{
	nb_queue *entry;
	nb_queue *next;
	NB_QUEUE_FOREACH_SAFE(entry, next, &client->messages) {
		struct runtime_client_message_node *node =
			NB_QUEUE_DATA(entry, struct runtime_client_message_node, node);

		if (node->message->kind != RUNTIME_MESSAGE_EVENT)
			continue;

		struct runtime_client_subscription *subscription =
			client_subscription_locked(client, node->message->session_id);
		if (subscription)
			subscription->suspended = 1;

		nb_queue_remove(entry);
		runtime_message_destroy(node->message);
		free(node);
	}

	client->message_events = 0;
	struct runtime_client_subscription *subscription =
		client_subscription_locked(client, session_id);
	if (subscription)
		subscription->suspended = 1;

	cJSON *data NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *detail = cJSON_CreateObject();
	if (!data || !detail) {
		cJSON_Delete(detail);
		return NB_ERR_NOMEM;
	}

	cJSON_AddStringToObject(data, "type", "runtime");
	cJSON_AddStringToObject(data, "name", "runtime.resync_required");
	cJSON_AddStringToObject(detail, "reason", "client_buffer_full");
	cJSON_AddBoolToObject(detail, "all_subscriptions", 1);
	cJSON_AddItemToObject(data, "data", detail);
	struct runtime_message *message NB_CLEANUP(runtime_message_cleanup) =
		runtime_client_message_create(RUNTIME_MESSAGE_EVENT, NULL,
			"runtime.resync_required", session_id, NULL, sequence, data, NB_ERR_AGAIN);
	if (!message)
		return NB_ERR_NOMEM;

	struct runtime_client_message_node *node = calloc(1, sizeof(*node));
	if (!node)
		return NB_ERR_NOMEM;

	node->message = message;
	message = NULL;
	nb_queue_insert_tail(&client->messages, &node->node);
	client->message_events++;
	client_signal_locked(client);
	return 0;
}

int runtime_client_enqueue_locked(struct runtime_client *client,
	struct runtime_message **message)
{
	if (client->closed)
		return NB_ERR_CLOSED;

	if ((*message)->kind == RUNTIME_MESSAGE_EVENT &&
	    client->message_events >= client->options.event_capacity)
		return client_event_overflow_locked(client, (*message)->session_id,
			(*message)->sequence);

	struct runtime_client_message_node *node = calloc(1, sizeof(*node));
	if (!node)
		return NB_ERR_NOMEM;

	node->message = *message;
	*message = NULL;
	nb_queue_insert_tail(&client->messages, &node->node);
	if (node->message->kind == RUNTIME_MESSAGE_EVENT)
		client->message_events++;

	client_signal_locked(client);
	return 0;
}

int runtime_client_respond_locked(struct runtime_client_request_node *request,
	int64_t session_id, const char *turn_id, cJSON **data, int error_code)
{
	struct runtime_message *message NB_CLEANUP(runtime_message_cleanup) =
		runtime_client_message_create(RUNTIME_MESSAGE_RESPONSE, request->id,
			request->method, session_id, turn_id, 0, data ? *data : NULL, error_code);

	if (!message)
		return NB_ERR_NOMEM;

	return runtime_client_enqueue_locked(request->client, &message);
}

struct runtime_client_lane *runtime_client_lane_find_locked(
	struct runtime_client_service *service, int64_t session_id)
{
	for (size_t i = 0; i < service->lanes.len; i++) {
		struct runtime_client_lane **lane = nb_vec_at(&service->lanes, i);

		if ((*lane)->session.id == session_id)
			return *lane;
	}

	return NULL;
}

static void request_destroy(struct runtime_client_request_node *request)
{
	if (!request)
		return;

	free(request->id);
	free(request->method);
	cJSON_Delete(request->params);
	if (request->retained) {
		struct runtime_client_service *service = request->client->service;

		nb_mutex_lock(service->lock);
		client_release_locked(request->client);
		int destroy = service_release_ready_locked(service);
		nb_mutex_unlock(service->lock);
		if (destroy)
			service_destroy(service);
	}

	free(request);
}

static unsigned request_capability(const char *method)
{
	if (!strcmp(method, "session.claim_interactions") ||
	    !strcmp(method, "interaction.progress"))
		return RUNTIME_CLIENT_INTERACT;

	if (!strcmp(method, "turn.start") || !strcmp(method, "turn.steer") ||
	    !strcmp(method, "turn.expedite") || !strcmp(method, "turn.interrupt") ||
	    !strcmp(method, "agent.followup") ||
	    !strcmp(method, "agent.interrupt") || !strcmp(method, "process.write") ||
	    !strcmp(method, "process.stop"))
		return RUNTIME_CLIENT_EXECUTE;

	if (strstr(method, ".get") || strstr(method, ".list") || strstr(method, ".read") ||
	    strstr(method, ".preview") || !strcmp(method, "runtime.info") ||
	    !strcmp(method, "runtime.capabilities") || !strcmp(method, "session.subscribe") ||
	    !strcmp(method, "session.unsubscribe") || !strcmp(method, "agent.activity") ||
	    !strcmp(method, "agent.budget") ||
	    !strcmp(method, "agent.subscribe") || !strcmp(method, "agent.events") ||
	    !strcmp(method, "agent.select") || !strcmp(method, "turn.inspect") ||
	    !strcmp(method, "history.diagnose"))
		return RUNTIME_CLIENT_READ;

	return RUNTIME_CLIENT_MANAGE;
}

static int request_is_control(const char *method)
{
	return !strcmp(method, "turn.interrupt") || !strcmp(method, "turn.steer") ||
		!strcmp(method, "turn.expedite") || !strcmp(method, "turn.get") ||
		!strcmp(method, "session.claim_interactions") ||
		!strcmp(method, "interaction.progress");
}

static int request_needs_actor(const char *method)
{
	return !strncmp(method, "permission.", 11) || !strncmp(method, "model.", 6) ||
		!strncmp(method, "process.", 8) || !strncmp(method, "agent.", 6) ||
		!strncmp(method, "checkpoint.", 11) || !strncmp(method, "memory.", 7) ||
		!strncmp(method, "preference.", 11) || !strncmp(method, "skill.", 6) ||
		!strncmp(method, "recap.", 6) || !strncmp(method, "hooks.", 6) ||
		!strcmp(method, "turn.inspect") ||
		!strncmp(method, "context.", 8) || !strcmp(method, "history.compress") ||
		!strcmp(method, "history.repair") || !strcmp(method, "history.diagnose") ||
		!strcmp(method, "session.settings.get") ||
		!strcmp(method, "session.settings.update");
}

static int request_safe_during_turn(const char *method)
{
	return !strcmp(method, "process.list") || !strcmp(method, "process.read") ||
		!strcmp(method, "process.stop") || !strcmp(method, "process.write") ||
		!strcmp(method, "permission.get") || !strcmp(method, "agent.list") ||
		!strcmp(method, "agent.read") || !strcmp(method, "agent.followup") ||
		!strcmp(method, "agent.interrupt") || !strcmp(method, "agent.activity") ||
		!strcmp(method, "agent.budget") ||
		!strcmp(method, "agent.subscribe") || !strcmp(method, "agent.events") ||
		!strcmp(method, "agent.select");
}

static int lane_background_work_locked(struct runtime_client_lane *lane)
{
	return lane->actor && (exec_jobs_active(lane->actor->jobs) ||
		agent_control_active(lane->actor->context.agents, 0));
}

static int request_safe_with_background(const char *method)
{
	return request_safe_during_turn(method) ||
		request_capability(method) == RUNTIME_CLIENT_READ ||
		!strncmp(method, "recap.", 6);
}

static int request_reads_manager_metadata(const char *method)
{
	return !strcmp(method, "runtime.info") || !strcmp(method, "runtime.capabilities") ||
		!strcmp(method, "session.list") || !strcmp(method, "session.get") ||
		!strcmp(method, "history.list") || !strcmp(method, "history.read");
}

static int dispatch_domain(struct runtime_client_service *service, struct runtime *target,
	struct runtime_client_request_node *request, cJSON **result)
{
	struct mcp_client *client = NULL;
	int (*previous_check)(void *) = NULL;
	void *previous_data = NULL;
	if (!strcmp(request->method, "mcp.connect") ||
	    !strcmp(request->method, "mcp.discover")) {
		client = mcp_registry_borrow(&target->context.mcp,
			runtime_client_json_string(request->params, "name"));
		struct runtime_mcp_job *job = runtime_mcp_job_find(target, client);
		if (job) {
			nb_mutex_lock(job->lock);
			int active = job->active;
			nb_mutex_unlock(job->lock);
			if (active)
				client = NULL;
		}

		if (client) {
			previous_check = client->interrupt_check;
			previous_data = client->interrupt_user_data;
			client->interrupt_check = runtime_client_service_interrupted;
			client->interrupt_user_data = service;
		}
	}

	int rc = runtime_client_service_interrupted(service) ? NB_ERR_CLOSED :
		runtime_protocol_dispatch(target, request->method, request->params, result);
	if (client) {
		client->interrupt_check = previous_check;
		client->interrupt_user_data = previous_data;
	}

	return rc;
}

static int subscribe_request(struct runtime_client_request_node *request,
	struct runtime_client_lane *lane)
{
	struct runtime_client_service *service = request->client->service;
	cJSON *snapshot NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	nb_mutex_lock(service->lock);
	int rc = runtime_client_subscribe_locked(request->client, lane->session.id);
	if (rc == 0) {
		snapshot = runtime_client_lane_snapshot_locked(lane);
		if (!snapshot)
			rc = NB_ERR_NOMEM;
	}

	if (snapshot) {
		cJSON_AddStringToObject(snapshot, "runtime_epoch", service->epoch);
		cJSON_AddNumberToObject(snapshot, "cursor", (double)lane->sequence);
		const char *epoch = runtime_client_json_string(request->params, "runtime_epoch");
		int64_t since = runtime_client_json_id(request->params, "since_sequence");
		uint64_t oldest = lane->sequence;
		if (!nb_queue_empty(&lane->replay)) {
			struct runtime_client_message_node *first = NB_QUEUE_DATA(
				nb_queue_head(&lane->replay),
				struct runtime_client_message_node, node);
			oldest = first->message->sequence;
		}

		cJSON_AddBoolToObject(snapshot, "resync_required",
			(epoch[0] && strcmp(epoch, service->epoch)) ||
			(since > 0 && (uint64_t)since + 1 < oldest));
	}

	(void)runtime_client_respond_locked(request, lane->session.id, NULL, &snapshot, rc);
	nb_mutex_unlock(service->lock);
	return rc;
}

static int unsubscribe_request(struct runtime_client_request_node *request, int64_t session_id)
{
	struct runtime_client_service *service = request->client->service;
	nb_mutex_lock(service->lock);
	for (size_t i = 0; i < request->client->subscriptions.len; i++) {
		struct runtime_client_subscription *subscription =
			nb_vec_at(&request->client->subscriptions, i);

		if (subscription->session_id == session_id) {
			nb_vec_remove(&request->client->subscriptions, i);
			break;
		}
	}

	(void)runtime_client_respond_locked(request, session_id, NULL, NULL, 0);
	nb_mutex_unlock(service->lock);
	return 0;
}

static int lane_settings_read(struct runtime_client_lane *lane,
	struct runtime_client_request_node *request, cJSON **out)
{
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc = runtime_protocol_dispatch(lane->service->owner, request->method,
		request->params, &json);
	if (rc != 0)
		return rc;

	if (!strcmp(request->method, "permission.get") && lane->has_permission) {
		const struct permission_policy *policy = &lane->permission;
		if (!cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(json, "access"),
		    permission_access_name(policy->access)) ||
		    !cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(json, "approval"),
		    permission_approval_name(policy->approval)) ||
		    !cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(json, "reviewer"),
		    permission_reviewer_name(policy->reviewer)))
			return NB_ERR_NOMEM;
	}

	if (!strcmp(request->method, "model.get")) {
		if (lane->session.model[0] && !cJSON_SetValuestring(
		    cJSON_GetObjectItemCaseSensitive(json, "model"), lane->session.model))
			return NB_ERR_NOMEM;

		if (lane->has_ultra) {
			cJSON_DeleteItemFromObjectCaseSensitive(json, "ultra");
			if (!cJSON_AddBoolToObject(json, "ultra", lane->ultra))
				return NB_ERR_NOMEM;
		}
	}

	*out = json;
	json = NULL;
	return 0;
}

static void dispatch_request(struct runtime_client_request_node *request)
{
	struct runtime_client_service *service = request->client->service;
	struct runtime_client_lane *lane = NULL;
	struct runtime *target = service->owner;
	int64_t session_id = runtime_client_json_id(request->params, "session_id");
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc = 0;
	int settings_snapshot = 0;
	struct runtime_client_lane *managed_lane = NULL;
	unsigned capability = request_capability(request->method);

	if (!(request->client->options.capabilities & capability))
		rc = NB_ERR_NOT_PERMITTED;

	if (rc == 0 && !strcmp(request->method, "session.unsubscribe")) {
		(void)unsubscribe_request(request, session_id);
		return;
	}

	if (rc == 0 && !strcmp(request->method, "interaction.progress")) {
		(void)runtime_client_interaction_progress(request);
		return;
	}

	int lane_request = !strncmp(request->method, "turn.", 5) ||
		!strcmp(request->method, "session.subscribe") ||
		!strcmp(request->method, "session.claim_interactions") ||
		!strcmp(request->method, "session.load") ||
		!strcmp(request->method, "session.unload") ||
		request_needs_actor(request->method) ||
		(session_id > 0 && !strcmp(request->method, "task.execute"));
	if ((!strcmp(request->method, "permission.update") ||
	    !strcmp(request->method, "model.update") ||
	    !strcmp(request->method, "permission.get") ||
	    !strcmp(request->method, "model.get")) &&
	    !strcmp(runtime_client_json_string(request->params, "scope"), "defaults"))
		lane_request = 0;

	if (rc == 0 && lane_request)
		rc = runtime_client_lane_create(service, session_id, &lane);

	if (rc == 0 && !strncmp(request->method, "turn.", 5) &&
	    strcmp(request->method, "turn.inspect")) {
		(void)runtime_client_turn_command(request, lane);
		return;
	}

	if (rc == 0 && !strcmp(request->method, "session.subscribe")) {
		(void)subscribe_request(request, lane);
		return;
	}

	if (rc == 0 && !strcmp(request->method, "session.claim_interactions")) {
		(void)runtime_client_claim_interactions(request, lane);
		return;
	}

	if (rc == 0 && lane) {
		nb_mutex_lock(service->lock);
		int active = lane->working ||
			(lane_background_work_locked(lane) &&
			 !request_safe_with_background(request->method));
		int managing = lane->managing || lane->actor_opening || lane->controls > 0;
		settings_snapshot = !lane->actor && (!strcmp(request->method, "permission.get") ||
			!strcmp(request->method, "model.get"));
		if (!managing && (!active || request_safe_during_turn(request->method))) {
			lane->managing = 1;
			managed_lane = lane;
			if (capability != RUNTIME_CLIENT_READ)
				lane->management_controller = request->client;
		}

		nb_mutex_unlock(service->lock);
		if (managing || (active && !request_safe_during_turn(request->method)))
			rc = NB_ERR_BUSY;
		else if (!strcmp(request->method, "session.unload")) {
			nb_mutex_lock(service->lock);
			struct runtime *actor = lane->actor;

			lane->actor = NULL;
			nb_mutex_unlock(service->lock);
			runtime_close(actor);
		} else if (settings_snapshot) {
			rc = lane_settings_read(lane, request, &result);
		} else {
			rc = runtime_client_lane_ensure_actor(lane);
			target = lane->actor;
		}
	}


	if (rc == 0 && !strcmp(request->method, "session.load"))
		result = cJSON_CreateObject();
	else if (rc == 0 && !strcmp(request->method, "session.unload"))
		result = cJSON_CreateObject();
	else if (rc == 0 && !settings_snapshot) {
		if ((!strcmp(request->method, "session.delete") ||
		     !strcmp(request->method, "session.update")) && session_id > 0) {
			struct runtime_client_lane *existing = NULL;
			rc = runtime_client_lane_create(service, session_id, &existing);
			nb_mutex_lock(service->lock);
			if (rc == 0 && (existing->working || existing->managing ||
			    existing->controls ||
			    existing->actor_opening || lane_background_work_locked(existing)))
				rc = NB_ERR_BUSY;
			else if (rc == 0) {
				existing->managing = 1;
				managed_lane = existing;
			}

			nb_mutex_unlock(service->lock);
		}

		if (rc == 0 && lane && !strcmp(request->method, "checkpoint.restore"))
			rc = runtime_client_mutation_begin(lane, "checkpoint");

		nb_mutex_lock(service->lock);
		if (rc == 0 && service->stopping)
			rc = NB_ERR_CLOSED;
		else if (rc == 0 && lane && lane->actor &&
		    !strcmp(request->method, "history.compress")) {
			struct react_context *react = lane->actor->context.react;

			nb_atomic_int_store(&react->cancelled, 0);
			nb_atomic_int_store(&react->preserve_background_on_cancel, 0);
			nb_cancel_reset(&react->cancel_token);
		}

		nb_mutex_unlock(service->lock);
		int combined = (!strcmp(request->method, "permission.update") ||
			!strcmp(request->method, "model.update")) &&
			!strcmp(runtime_client_json_string(request->params, "scope"),
				"session_and_defaults");
		if (rc == 0 && ((target == service->owner &&
		    !request_reads_manager_metadata(request->method)) || combined)) {
			nb_mutex_lock(service->domain_lock);
			rc = dispatch_domain(service, target, request, &result);
			nb_mutex_unlock(service->domain_lock);
		} else if (rc == 0) {
			rc = dispatch_domain(service, target, request, &result);
		}
	}

	if (rc == 0 && lane && (!strcmp(request->method, "permission.update") ||
	    !strcmp(request->method, "model.update"))) {
		if (!strcmp(request->method, "permission.update")) {
			rc = tool_context_get_permission_policy(lane->actor->context.tctx,
				&lane->permission);
			if (rc == 0)
				lane->has_permission = 1;
		} else {
			const cJSON *ultra =
				cJSON_GetObjectItemCaseSensitive(request->params, "ultra");
			if (cJSON_IsBool(ultra)) {
				lane->ultra = cJSON_IsTrue(ultra);
				lane->has_ultra = 1;
			}

			struct session refreshed;
			if (session_get_by_id(&service->owner->context.database, session_id,
			    &refreshed) == 0) {
				nb_mutex_lock(service->lock);
				lane->session = refreshed;
				nb_mutex_unlock(service->lock);
			}
		}

		if (rc == 0)
			rc = runtime_client_lane_settings_store(lane);
	}

	if (rc == 0 && session_id > 0 && (!strcmp(request->method, "session.update") ||
	    !strcmp(request->method, "session.delete"))) {
		nb_mutex_lock(service->lock);
		struct runtime_client_lane *existing =
			runtime_client_lane_find_locked(service, session_id);
		struct runtime *actor = existing ? existing->actor : NULL;
		if (existing)
			existing->actor = NULL;

		nb_mutex_unlock(service->lock);
		runtime_close(actor);
		if (existing && !strcmp(request->method, "session.update")) {
			struct session refreshed;
			if (session_get_by_id(&service->owner->context.database, session_id,
			    &refreshed) == 0) {
				nb_mutex_lock(service->lock);
				existing->session = refreshed;
				nb_mutex_unlock(service->lock);
			}
		}
	}

	nb_mutex_lock(service->lock);
	if (managed_lane)
		managed_lane->managing = 0;

	if (managed_lane)
		managed_lane->management_controller = NULL;

	if (lane && !strcmp(request->method, "checkpoint.restore"))
		runtime_client_mutation_release_locked(lane);

	if (rc == 0 && !strcmp(request->method, "session.get") && result) {
		struct runtime_client_lane *existing =
			runtime_client_lane_find_locked(service, session_id);
		if (existing) {
			cJSON *snapshot = runtime_client_lane_snapshot_locked(existing);
			if (snapshot)
				cJSON_AddItemToObject(result, "runtime", snapshot);
		}
	}

	(void)runtime_client_respond_locked(request, session_id, NULL, &result, rc);
	nb_mutex_unlock(service->lock);
}

static int request_needs_management(const char *method)
{
	return strcmp(method, "runtime.info") && strcmp(method, "runtime.capabilities") &&
		strcmp(method, "session.list") &&
		strcmp(method, "session.get") && strcmp(method, "session.subscribe") &&
		strcmp(method, "session.unsubscribe") && strcmp(method, "history.list") &&
		strcmp(method, "history.read");
}

static void *client_management_main(void *data)
{
	struct runtime_client_service *service = data;

	for (;;) {
		nb_mutex_lock(service->lock);
		while (nb_queue_empty(&service->management_requests) && !service->stopping)
			nb_cond_wait(service->changed, service->lock);

		if (nb_queue_empty(&service->management_requests) && service->stopping) {
			nb_mutex_unlock(service->lock);
			break;
		}

		struct runtime_client_request_node *request = NB_QUEUE_DATA(
			nb_queue_head(&service->management_requests),
			struct runtime_client_request_node, node);
		nb_queue_remove(&request->node);
		int stopping = service->stopping;
		if (stopping)
			(void)runtime_client_respond_locked(request, 0, NULL, NULL, NB_ERR_CLOSED);

		nb_mutex_unlock(service->lock);
		if (!stopping)
			dispatch_request(request);

		request_destroy(request);
	}

	return NULL;
}

static int management_workers_start_locked(struct runtime_client_service *service)
{
	while (service->management_worker_count < RUNTIME_CLIENT_MANAGEMENT_LIMIT) {
		int index = service->management_worker_count;
		int rc = nb_thread_create(&service->management_workers[index],
			client_management_main, service, NULL);

		if (rc != 0)
			return service->management_worker_count ? 0 : rc;

		service->management_worker_count++;
	}

	return 0;
}

static void *client_dispatch_main(void *data)
{
	struct runtime_client_service *service = data;

	for (;;) {
		nb_mutex_lock(service->lock);
		while (nb_queue_empty(&service->requests) && !service->stopping)
			nb_cond_wait(service->changed, service->lock);

		if (nb_queue_empty(&service->requests) && service->stopping) {
			nb_mutex_unlock(service->lock);
			break;
		}

		struct runtime_client_request_node *request = NB_QUEUE_DATA(
			nb_queue_head(&service->requests),
			struct runtime_client_request_node, node);
		nb_queue_remove(&request->node);
		int stopping = service->stopping;
		if (stopping)
			(void)runtime_client_respond_locked(request, 0, NULL, NULL, NB_ERR_CLOSED);

		if (!stopping && request_needs_management(request->method)) {
			int rc = management_workers_start_locked(service);

			if (rc == 0) {
				nb_queue_insert_tail(&service->management_requests, &request->node);
				nb_cond_broadcast(service->changed);
				request = NULL;
			} else {
				(void)runtime_client_respond_locked(request, 0, NULL, NULL, rc);
				stopping = 1;
			}
		}

		nb_mutex_unlock(service->lock);
		if (!stopping && request)
			dispatch_request(request);

		request_destroy(request);
	}

	return NULL;
}

int runtime_clients_init(struct runtime *runtime)
{
	struct runtime_client_service *service = calloc(1, sizeof(*service));

	if (!service)
		return NB_ERR_NOMEM;

	service->owner = runtime;
	service->lock = nb_mutex_new(0);
	service->domain_lock = nb_mutex_new(0);
	service->changed = nb_cond_new();
	nb_queue_init(&service->clients);
	nb_queue_init(&service->requests);
	nb_queue_init(&service->management_requests);
	struct runtime_client_lane *lane = NULL;
	nb_vec_init(&service->lanes, sizeof(lane));
	if (!service->lock || !service->domain_lock || !service->changed) {
		nb_mutex_free(service->lock);
		nb_mutex_free(service->domain_lock);
		nb_cond_free(service->changed);
		free(service);
		return NB_ERR_NOMEM;
	}

	nb_buf epoch NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf_init(&epoch);
	int rc = nb_random_hex(&epoch, 16, NULL);
	if (rc == 0)
		snprintf(service->epoch, sizeof(service->epoch), "%s", nb_buf_cstr(&epoch));
	else
		snprintf(service->epoch, sizeof(service->epoch), "%lld",
			(long long)nb_time_monotonic_ns());

	runtime->clients = service;
	return 0;
}

static void client_destroy(struct runtime_client *client)
{
	message_queue_clear(&client->messages);
	nb_vec_destroy(&client->subscriptions);
	nb_map_free(client->requests);
	free(client->user_id);
	free(client);
}

static int service_release_ready_locked(struct runtime_client_service *service)
{
	if (!service->owner_released || service->destroying || service->receivers > 0)
		return 0;

	if (!nb_queue_empty(&service->clients))
		return 0;

	service->destroying = 1;
	return 1;
}

static void service_destroy(struct runtime_client_service *service)
{
	while (!nb_queue_empty(&service->clients)) {
		struct runtime_client *client = NB_QUEUE_DATA(nb_queue_head(&service->clients),
			struct runtime_client, node);

		nb_queue_remove(&client->node);
		client_destroy(client);
	}

	nb_vec_destroy(&service->lanes);
	nb_cond_free(service->changed);
	nb_mutex_free(service->lock);
	nb_mutex_free(service->domain_lock);
	free(service);
}

void runtime_clients_shutdown(struct runtime *runtime)
{
	struct runtime_client_service *service = runtime ? runtime->clients : NULL;

	if (!service)
		return;

	runtime->clients = NULL;
	nb_mutex_lock(service->lock);
	service->stopping = 1;
	react_cancel(runtime->context.react);
	for (size_t i = 0; i < service->lanes.len; i++) {
		struct runtime_client_lane **lane = nb_vec_at(&service->lanes, i);

		(*lane)->stop = 1;
		if ((*lane)->actor)
			react_cancel((*lane)->actor->context.react);

		if ((*lane)->turn)
			(*lane)->turn->cancelled = 1;

		runtime_client_interactions_cancel_locked(*lane);
	}

	nb_cond_broadcast(service->changed);
	nb_mutex_unlock(service->lock);
	if (service->dispatcher)
		(void)nb_thread_join_take(&service->dispatcher, NULL);

	for (int i = 0; i < service->management_worker_count; i++)
		(void)nb_thread_join_take(&service->management_workers[i], NULL);

	nb_mutex_lock(service->lock);
	while (service->direct_dispatches > 0)
		nb_cond_wait(service->changed, service->lock);

	nb_mutex_unlock(service->lock);
	for (size_t i = 0; i < service->lanes.len; i++) {
		struct runtime_client_lane **lane = nb_vec_at(&service->lanes, i);

		nb_mutex_lock(service->lock);
		struct runtime *actor = (*lane)->actor;
		nb_mutex_unlock(service->lock);
		if (actor)
			runtime_cancel_turn(actor);
	}

	for (int i = 0; i < service->turn_worker_count; i++)
		(void)nb_thread_join_take(&service->turn_workers[i], NULL);

	nb_vec lanes NB_CLEANUP(nb_vec_cleanup) = {0};
	struct runtime_client_lane *lane_owner = NULL;
	nb_vec_init(&lanes, sizeof(lane_owner));
	nb_mutex_lock(service->lock);
	nb_vec_move(&lanes, &service->lanes);
	nb_mutex_unlock(service->lock);
	for (size_t i = 0; i < lanes.len; i++) {
		struct runtime_client_lane **lane = nb_vec_at(&lanes, i);

		runtime_client_lane_destroy(*lane);
	}

	runtime_client_turn_storage_close(service);
	nb_mutex_lock(service->lock);
	service->owner = NULL;
	service->owner_released = 1;
	nb_queue *entry;
	NB_QUEUE_FOREACH(entry, &service->clients) {
		struct runtime_client *client = NB_QUEUE_DATA(entry, struct runtime_client, node);

		client->closed = 1;
	}

	nb_cond_broadcast(service->changed);
	int destroy = service_release_ready_locked(service);
	nb_mutex_unlock(service->lock);
	if (destroy)
		service_destroy(service);
}

int runtime_client_open(struct runtime *runtime,
	const struct runtime_client_options *options, struct runtime_client **out)
{
	if (!runtime || !runtime->clients || !out)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	struct runtime_client *client = calloc(1, sizeof(*client));
	if (!client)
		return NB_ERR_NOMEM;

	client->service = runtime->clients;
	client->references = 1;
	if (options)
		client->options = *options;

	if (!client->options.capabilities)
		client->options.capabilities = RUNTIME_CLIENT_ALL;

	if (!client->options.request_capacity)
		client->options.request_capacity = RUNTIME_CLIENT_REQUEST_LIMIT;

	if (!client->options.event_capacity)
		client->options.event_capacity = RUNTIME_CLIENT_EVENT_LIMIT;

	client->user_id = strdup(options && options->user_id ? options->user_id : "local");
	client->requests = nb_map_new();
	nb_queue_init(&client->messages);
	struct runtime_client_subscription subscription = {0};
	nb_vec_init(&client->subscriptions, sizeof(subscription));
	if (!client->user_id || !client->requests) {
		client_destroy(client);
		return NB_ERR_NOMEM;
	}

	client->options.user_id = client->user_id;
	struct runtime_client_service *service = client->service;
	nb_mutex_lock(service->lock);
	int rc = service->stopping ? NB_ERR_CLOSED : 0;
	if (rc == 0 && !service->dispatcher) {
		rc = runtime_client_turn_storage_init(service);
		if (rc == 0) {
			service->storage_ready = 1;
			rc = nb_thread_create(&service->dispatcher, client_dispatch_main,
				service, NULL);
		}
	}

	if (rc == 0)
		nb_queue_insert_tail(&service->clients, &client->node);

	nb_mutex_unlock(service->lock);
	if (rc != 0) {
		client_destroy(client);
		return rc;
	}

	*out = client;
	return 0;
}

int runtime_client_request_copy(struct runtime_client *client,
	const struct runtime_command *command)
{
	if (!client || !command || !command->id || !command->id[0] ||
	    !command->method || !command->method[0] ||
	    (command->params && !cJSON_IsObject(command->params)))
		return NB_ERR_INVAL;

	struct runtime_client_request_node *request = calloc(1, sizeof(*request));
	if (!request)
		return NB_ERR_NOMEM;

	request->client = client;
	request->id = strdup(command->id);
	request->method = strdup(command->method);
	request->params = command->params ? cJSON_Duplicate(command->params, 1) :
		cJSON_CreateObject();
	if (!request->id || !request->method || !request->params) {
		request_destroy(request);
		return NB_ERR_NOMEM;
	}

	struct runtime_client_service *service = client->service;
	nb_mutex_lock(service->lock);
	int rc = client->closed || service->stopping ? NB_ERR_CLOSED : 0;
	size_t capacity = client->options.request_capacity;
	if (request_is_control(command->method) && capacity <= SIZE_MAX - 16)
		capacity += 16;

	if (rc == 0 && client->pending_requests >= capacity)
		rc = NB_ERR_BUSY;

	if (rc == 0 && nb_map_has_cstr(client->requests, command->id))
		rc = NB_ERR_EXISTS;

	if (rc == 0 && !nb_map_put_cstr(client->requests, command->id, client, NULL))
		rc = NB_ERR_NOMEM;

	if (rc == 0) {
		client->pending_requests++;
		client->references++;
		request->retained = 1;
		if (strcmp(command->method, "turn.start") &&
		    strcmp(command->method, "turn.interrupt") &&
		    strcmp(command->method, "turn.steer") &&
		    strcmp(command->method, "turn.expedite") &&
		    strcmp(command->method, "turn.get") &&
		    strcmp(command->method, "interaction.progress") &&
		    strcmp(command->method, "session.claim_interactions")) {
			nb_queue_insert_tail(&service->requests, &request->node);
			request = NULL;
			nb_cond_broadcast(service->changed);
		} else {
			service->direct_dispatches++;
		}
	}

	nb_mutex_unlock(service->lock);
	if (rc == 0 && request) {
		dispatch_request(request);
		nb_mutex_lock(service->lock);
		service->direct_dispatches--;
		nb_cond_broadcast(service->changed);
		nb_mutex_unlock(service->lock);
	}

	request_destroy(request);

	return rc;
}

int runtime_client_receive(struct runtime_client *client, int timeout_ms,
	struct runtime_message **out)
{
	if (!client || !out)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	struct runtime_client_service *service = client->service;
	int64_t deadline = timeout_ms < 0 ? 0 : nb_time_monotonic_ns() +
		(int64_t)timeout_ms * INT64_C(1000000);
	nb_mutex_lock(service->lock);
	service->receivers++;
	client->references++;
	int rc = 0;
	while (nb_queue_empty(&client->messages) && !client->closed) {
		if (timeout_ms == 0) {
			rc = NB_ERR_AGAIN;
			break;
		}

		if (timeout_ms < 0)
			nb_cond_wait(service->changed, service->lock);
		else {
			rc = nb_cond_wait_until(service->changed, service->lock, deadline);
			if (rc != 0)
				break;
		}
	}

	if (rc == 0 && nb_queue_empty(&client->messages))
		rc = NB_ERR_CLOSED;

	if (rc == 0) {
		struct runtime_client_message_node *node = NB_QUEUE_DATA(
			nb_queue_head(&client->messages), struct runtime_client_message_node, node);

		nb_queue_remove(&node->node);
		*out = node->message;
		if ((*out)->kind == RUNTIME_MESSAGE_EVENT)
			client->message_events--;
		else if ((*out)->kind == RUNTIME_MESSAGE_RESPONSE) {
			(void)nb_map_del_cstr(client->requests, (*out)->id);
			if (client->pending_requests > 0)
				client->pending_requests--;
		}

		free(node);
	}

	service->receivers--;
	client_release_locked(client);
	int destroy = service_release_ready_locked(service);
	nb_mutex_unlock(service->lock);
	if (destroy)
		service_destroy(service);

	return rc;
}

int runtime_client_try_receive(struct runtime_client *client, struct runtime_message **out)
{
	return runtime_client_receive(client, 0, out);
}

void runtime_client_close(struct runtime_client *client)
{
	if (!client)
		return;

	struct runtime_client_service *service = client->service;
	nb_mutex_lock(service->lock);
	client->closed = 1;
	client->released = 1;
	client->options.wakeup = NULL;
	client->options.wakeup_user_data = NULL;
	message_queue_clear(&client->messages);
	client->message_events = 0;
	nb_map_clear(client->requests);
	client->pending_requests = 0;
	for (size_t i = 0; i < service->lanes.len; i++) {
		struct runtime_client_lane **lane = nb_vec_at(&service->lanes, i);

		if ((*lane)->turn && (*lane)->turn->controller == client)
			(*lane)->turn->controller = NULL;

		if ((*lane)->management_controller == client)
			(*lane)->management_controller = NULL;

		if ((*lane)->interaction_controller == client)
			(*lane)->interaction_controller = NULL;

		nb_queue *entry;
		NB_QUEUE_FOREACH(entry, &(*lane)->interactions) {
			struct runtime_client_interaction *interaction =
				NB_QUEUE_DATA(entry, struct runtime_client_interaction, node);

			if (interaction->controller == client)
				interaction->controller = NULL;
		}
	}

	nb_cond_broadcast(service->changed);
	client_release_locked(client);
	int destroy = service_release_ready_locked(service);
	nb_mutex_unlock(service->lock);
	if (destroy)
		service_destroy(service);
}
