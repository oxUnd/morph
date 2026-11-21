#include "runtime/client_internal.h"

#include "runtime/runtime_internal.h"
#include "runtime/protocol_dispatch.h"
#include "persistence/runtime_store.h"
#include "permission/context.h"
#include "agent/tools/exec_jobs.h"
#include <nb/cleanup.h>
#include <nb/time.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int runtime_client_turn_storage_init(struct runtime_client_service *service)
{
	struct db *db = &service->owner->context.database;
	struct runtime_store_instances instances NB_CLEANUP(runtime_store_instances_cleanup) = {0};
	int rc = runtime_store_init(db);

	if (rc == 0)
		rc = runtime_store_pending_instances_copy(db, &instances);

	if (rc != 0)
		return rc;

	for (size_t i = 0; i < instances.count; i++) {
		const struct runtime_store_instance *instance = &instances.items[i];
		int orphan = instance->missing || instance->closed || instance->process_id <= 0;

		if (!orphan && instance->process_id <= INT_MAX &&
		    kill((pid_t)instance->process_id, 0) != 0)
			orphan = errno == ESRCH;

		if (orphan) {
			rc = runtime_store_recover_epoch(db, instance->epoch, NB_ERR_INTERRUPTED);
			if (rc != 0)
				return rc;
		}
	}

	return runtime_store_instance_register(db, service->epoch, (int64_t)getpid());
}

void runtime_client_turn_storage_close(struct runtime_client_service *service)
{
	if (!service->storage_ready)
		return;

	(void)runtime_store_instance_close(&service->owner->context.database, service->epoch);
}

int runtime_client_lane_settings_store(struct runtime_client_lane *lane)
{
	struct runtime_store_session_settings settings = {
		.session_id = lane->session.id,
		.has_permission = lane->has_permission,
		.access = (int)lane->permission.access,
		.approval = (int)lane->permission.approval,
		.reviewer = (int)lane->permission.reviewer,
		.has_ultra = lane->has_ultra,
		.ultra = lane->ultra,
	};

	return runtime_store_session_settings_put(&lane->service->owner->context.database,
		&settings);
}

static void lane_settings_load(struct runtime_client_lane *lane)
{
	struct runtime_store_session_settings settings = {0};

	if (runtime_store_session_settings_get(&lane->service->owner->context.database,
	    lane->session.id, &settings) != 0)
		return;

	if (settings.has_permission) {
		lane->permission.access = (enum permission_access)settings.access;
		lane->permission.approval = (enum permission_approval)settings.approval;
		lane->permission.reviewer = (enum permission_reviewer)settings.reviewer;
		lane->has_permission = permission_policy_validate(&lane->permission) == 0;
	}

	if (settings.has_ultra) {
		lane->ultra = settings.ultra != 0;
		lane->has_ultra = 1;
	}
}

int runtime_client_turn_store_locked(struct runtime_client_lane *lane)
{
	struct runtime_client_turn *turn = lane->turn;
	struct runtime_store_turn_input input = {
		.turn_id = turn->id,
		.session_id = lane->session.id,
		.owner_epoch = lane->service->epoch,
		.user_id = turn->user_id,
		.idempotency_key = turn->idempotency_key,
		.input = turn->input,
		.stored_input = turn->stored_input,
		.status = turn->status,
		.final_text = turn->final_text,
		.execution_rc = turn->execution_rc,
		.persistence_rc = turn->persistence_rc,
	};

	return runtime_store_turn_put(&lane->service->owner->context.database, &input);
}

static cJSON *turn_record_json(const struct runtime_store_turn_record *record)
{
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

	if (!json || !cJSON_AddStringToObject(json, "turn_id", record->turn_id) ||
	    !cJSON_AddStringToObject(json, "status", record->status) ||
	    !cJSON_AddStringToObject(json, "final_text", record->final_text) ||
	    !cJSON_AddNumberToObject(json, "execution_rc", record->execution_rc) ||
	    !cJSON_AddNumberToObject(json, "persistence_rc", record->persistence_rc) ||
	    !cJSON_AddNumberToObject(json, "session_id", (double)record->session_id))
		return NULL;

	cJSON *result = json;

	json = NULL;
	return result;
}

cJSON *runtime_client_turn_read(struct runtime_client_service *service,
	int64_t session_id, const char *turn_id)
{
	struct runtime_store_turn_record record NB_CLEANUP(runtime_store_turn_record_cleanup) = {0};
	int rc = runtime_store_turn_copy(&service->owner->context.database,
		session_id, turn_id, &record);

	return rc == 0 ? turn_record_json(&record) : NULL;
}

static cJSON *turn_idempotency_copy(struct runtime_client_lane *lane,
	const char *user_id, const char *key)
{
	if (!key[0])
		return NULL;

	struct runtime_store_turn_record record NB_CLEANUP(runtime_store_turn_record_cleanup) = {0};
	int rc = runtime_store_turn_by_key_copy(&lane->service->owner->context.database,
		lane->session.id, user_id, key, &record);

	return rc == 0 ? turn_record_json(&record) : NULL;
}

void runtime_client_turn_destroy(struct runtime_client_turn *turn)
{
	if (!turn)
		return;

	free(turn->id);
	free(turn->idempotency_key);
	free(turn->input);
	free(turn->stored_input);
	free(turn->user_id);
	free(turn->status);
	free(turn->final_text);
	free(turn->error_detail);
	free(turn);
}

static void turn_status_set(struct runtime_client_turn *turn, const char *status)
{
	char *copy = strdup(status);

	if (copy) {
		free(turn->status);
		turn->status = copy;
	}
}

static cJSON *turn_snapshot_locked(struct runtime_client_lane *lane)
{
	struct runtime_client_turn *turn = lane->turn;
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

	if (!json)
		return NULL;

	if (!cJSON_AddNumberToObject(json, "session_id", (double)lane->session.id) ||
	    !cJSON_AddStringToObject(json, "turn_id", turn ? turn->id : "") ||
	    !cJSON_AddStringToObject(json, "status", turn ? turn->status : "idle") ||
	    !cJSON_AddStringToObject(json, "final_text",
		turn && turn->final_text ? turn->final_text : "") ||
	    !cJSON_AddNumberToObject(json, "execution_rc", turn ? turn->execution_rc : 0) ||
	    !cJSON_AddNumberToObject(json, "persistence_rc", turn ? turn->persistence_rc : 0) ||
	    !cJSON_AddStringToObject(json, "error_detail",
		turn && turn->error_detail ? turn->error_detail : "") ||
	    !cJSON_AddNumberToObject(json, "sequence", (double)lane->sequence) ||
	    !cJSON_AddStringToObject(json, "stream_text", nb_buf_cstr(&lane->stream_text)))
		return NULL;

	cJSON *result = json;
	json = NULL;
	return result;
}

cJSON *runtime_client_lane_snapshot_locked(struct runtime_client_lane *lane)
{
	cJSON *snapshot NB_CLEANUP(cJSON_DeleteOwner) = turn_snapshot_locked(lane);
	cJSON *events = cJSON_CreateArray();
	cJSON *interactions = cJSON_CreateArray();
	if (!snapshot || !events || !interactions) {
		cJSON_Delete(events);
		cJSON_Delete(interactions);
		return NULL;
	}

	cJSON_AddItemToObject(snapshot, "events", events);
	cJSON_AddItemToObject(snapshot, "pending_interactions", interactions);
	cJSON_AddStringToObject(snapshot, "name", lane->session.name);
	cJSON_AddStringToObject(snapshot, "display_id", lane->session.display_id);
	cJSON_AddStringToObject(snapshot, "workdir", lane->session.workdir);
	cJSON_AddStringToObject(snapshot, "model", lane->session.model);
	nb_queue *entry;
	NB_QUEUE_FOREACH(entry, &lane->replay) {
		struct runtime_client_message_node *node =
			NB_QUEUE_DATA(entry, struct runtime_client_message_node, node);
		cJSON *copy = cJSON_Duplicate(node->message->data, 1);

		if (!copy)
			return NULL;

		cJSON_AddItemToArray(events, copy);
	}

	NB_QUEUE_FOREACH(entry, &lane->interactions) {
		struct runtime_client_interaction *interaction =
			NB_QUEUE_DATA(entry, struct runtime_client_interaction, node);
		if (interaction->resolved)
			continue;

		cJSON *item = cJSON_CreateObject();
		cJSON *request = cJSON_Duplicate(interaction->request, 1);
		if (!item || !request) {
			cJSON_Delete(item);
			cJSON_Delete(request);
			return NULL;
		}

		cJSON_AddStringToObject(item, "interaction_id", interaction->id);
		cJSON_AddStringToObject(item, "kind", interaction->kind);
		cJSON_AddItemToObject(item, "request", request);
		cJSON_AddItemToArray(interactions, item);
	}

	cJSON *result = snapshot;
	snapshot = NULL;
	return result;
}

void runtime_client_lane_publish_locked(struct runtime_client_lane *lane,
	const char *name, const cJSON *event)
{
	lane->sequence++;
	const char *turn_id = runtime_client_json_string(event, "turn_id");
	struct runtime_message *message NB_CLEANUP(runtime_message_cleanup) =
		runtime_client_message_create(RUNTIME_MESSAGE_EVENT, NULL, name,
			lane->session.id, turn_id, lane->sequence, event, 0);
	if (!message)
		return;

	cJSON_AddNumberToObject(message->data, "sequence", (double)lane->sequence);
	cJSON_AddNumberToObject(message->data, "session_id", (double)lane->session.id);
	cJSON_AddStringToObject(message->data, "runtime_epoch", lane->service->epoch);
	struct runtime_client_message_node *saved = calloc(1, sizeof(*saved));
	if (saved) {
		saved->message = runtime_client_message_create(RUNTIME_MESSAGE_EVENT, NULL, name,
			lane->session.id, turn_id, lane->sequence, message->data, 0);
		if (saved->message) {
			nb_queue_insert_tail(&lane->replay, &saved->node);
			lane->replay_count++;
		} else {
			free(saved);
		}
	}

	while (lane->replay_count > RUNTIME_CLIENT_REPLAY_LIMIT) {
		struct runtime_client_message_node *first = NB_QUEUE_DATA(
			nb_queue_head(&lane->replay), struct runtime_client_message_node, node);

		nb_queue_remove(&first->node);
		runtime_message_destroy(first->message);
		free(first);
		lane->replay_count--;
	}

	nb_queue *entry;
	NB_QUEUE_FOREACH(entry, &lane->service->clients) {
		struct runtime_client *client = NB_QUEUE_DATA(entry, struct runtime_client, node);
		if (!runtime_client_is_subscribed_locked(client, lane->session.id))
			continue;

		struct runtime_message *copy NB_CLEANUP(runtime_message_cleanup) =
			runtime_client_message_create(message->kind, NULL, name, lane->session.id,
				turn_id, lane->sequence, message->data, 0);
		if (copy)
			(void)runtime_client_enqueue_locked(client, &copy);
	}
}

int runtime_client_lane_emit(struct runtime_client_lane *lane,
	const char *name, const char *phase, const cJSON *data)
{
	cJSON *event NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *copy = data ? cJSON_Duplicate(data, 1) : cJSON_CreateObject();
	if (!event || !copy) {
		cJSON_Delete(copy);
		return NB_ERR_NOMEM;
	}

	cJSON_AddStringToObject(event, "type", "react");
	cJSON_AddStringToObject(event, "name", name);
	cJSON_AddStringToObject(event, "phase", phase);
	cJSON_AddItemToObject(event, "data", copy);
	nb_mutex_lock(lane->service->lock);
	if (lane->turn)
		cJSON_AddStringToObject(event, "turn_id", lane->turn->id);

	runtime_client_lane_publish_locked(lane, name, event);
	nb_mutex_unlock(lane->service->lock);
	return 0;
}

int runtime_client_lane_event(const struct morph_event *event, void *data)
{
	struct runtime_client_lane *lane = data;
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc = morph_event_json_create(event, &json);

	if (rc != 0)
		return rc;

	nb_mutex_lock(lane->service->lock);
	const char *agent_id = runtime_client_json_string(event->data, "agent_id");
	if ((!agent_id[0] || !strcmp(agent_id, "/root")) && event->name &&
	    (!strcmp(event->name, "react.content.delta") ||
	    !strcmp(event->name, "react.final.delta"))) {
		const char *text = runtime_client_json_string(event->data, "text");
		if (!text[0])
			text = event->message ? event->message : "";

		(void)nb_buf_append_cstr(&lane->stream_text, text);
	}

	if ((!agent_id[0] || !strcmp(agent_id, "/root")) && event->name &&
	    (!strcmp(event->name, "react.content.end") ||
	     !strcmp(event->name, "react.thought.end") || !strcmp(event->name, "react.final")))
		nb_buf_clear(&lane->stream_text);

	runtime_client_lane_publish_locked(lane, event->name ? event->name : "runtime.event", json);
	if (!lane->working && event->name && (!strcmp(event->name, "process.completed") ||
	    !strcmp(event->name, "agent.completed")))
		runtime_client_mutation_release_locked(lane);

	nb_mutex_unlock(lane->service->lock);
	return 0;
}

int runtime_client_lane_ensure_actor(struct runtime_client_lane *lane)
{
	struct runtime_client_service *service = lane->service;
	nb_mutex_lock(service->lock);
	while (lane->actor_opening && !service->stopping)
		nb_cond_wait(service->changed, service->lock);

	if (lane->actor) {
		lane->last_used_ms = nb_time_monotonic_ms();
		nb_mutex_unlock(service->lock);
		return 0;
	}

	if (service->stopping) {
		nb_mutex_unlock(service->lock);
		return NB_ERR_CLOSED;
	}

	struct runtime_client_lane *victim = NULL;
	struct runtime *evicted = NULL;
	size_t resident = 0;
	for (size_t i = 0; i < service->lanes.len; i++) {
		struct runtime_client_lane **candidate = nb_vec_at(&service->lanes, i);

		if ((*candidate)->actor || (*candidate)->actor_opening)
			resident++;

		if (*candidate == lane || !(*candidate)->actor || (*candidate)->working ||
		    (*candidate)->managing || (*candidate)->actor_opening ||
		    (*candidate)->controls ||
		    exec_jobs_active((*candidate)->actor->jobs) ||
		    agent_control_active((*candidate)->actor->context.agents, 0))
			continue;

		if (!victim || (*candidate)->last_used_ms < victim->last_used_ms)
			victim = *candidate;
	}

	if (resident >= RUNTIME_CLIENT_SESSION_LIMIT && !victim) {
		nb_mutex_unlock(service->lock);
		return NB_ERR_RESOURCE;
	}

	if (resident >= RUNTIME_CLIENT_SESSION_LIMIT) {
		victim->managing = 1;
		evicted = victim->actor;
		victim->actor = NULL;
	}

	lane->actor_opening = 1;
	struct runtime *owner = service->owner;
	struct runtime_options options = owner->options;
	nb_mutex_unlock(service->lock);
	if (evicted) {
		runtime_close(evicted);
		nb_mutex_lock(service->lock);
		victim->managing = 0;
		runtime_client_mutation_release_locked(victim);
		nb_mutex_unlock(service->lock);
	}

	options.db_path = owner->context.database.path;
	options.config_path = owner->context.config_path;
	options.workdir = lane->session.workdir[0] ? lane->session.workdir : owner->context.workdir;
	options.resume_session_ref = lane->session.display_id;
	options.default_session = NULL;
	options.create_new_session = 0;
	options.restore_recent_session = 0;
	options.process_replica = 1;
	options.event_cb = runtime_client_lane_event;
	options.event_user_data = lane;
	options.hitl_cb = runtime_client_hitl;
	options.hitl_user_data = lane;
	options.ask_user_cb = runtime_client_question;
	options.ask_user_user_data = lane;
	options.operation_approval_cb = runtime_client_approval;
	options.operation_approval_user_data = lane;
	options.task_events = NULL;
	options.usage_observer = NULL;
	options.usage_observer_user_data = NULL;
	options.startup_error = NULL;
	options.img_annotate_pause_cb = NULL;
	options.img_annotate_resume_cb = NULL;
	options.img_annotate_user_data = NULL;
	options.enable_img_annotate = 0;
	struct runtime *actor = NULL;
	int rc = runtime_open_controlled(owner, &options, runtime_client_service_interrupted,
		service, &actor);

	if (rc == 0)
		agent_control_set_all_events(actor->context.agents, 1);

	if (rc == 0 && lane->session.model[0] &&
	    strcmp(lane->session.model, actor->context.config.models.text.model))
		rc = runtime_session_set_model(actor, lane->session.model);

	if (rc == 0) {
		struct permission_policy policy = actor->context.config.permissions;

		if (lane->has_permission) {
			policy = lane->permission;
			rc = tool_context_set_permission_policy(actor->context.tctx, &policy);
		} else if (runtime_permission_get_policy(owner, &policy) == 0) {
			rc = tool_context_set_permission_policy(actor->context.tctx, &policy);
		}

		if (rc == 0)
			actor->context.config.permissions = policy;

		if (rc == 0 && lane->has_ultra) {
			snprintf(actor->context.config.agents.multi_agent_mode,
				sizeof(actor->context.config.agents.multi_agent_mode), "%s",
				lane->ultra ? "proactive" : "explicit");
			agent_control_set_proactive(actor->context.agents, lane->ultra);
		}

		if (rc == 0) {
			actor->context.mutation_begin = runtime_client_mutation_begin;
			actor->context.mutation_user_data = lane;
			rc = runtime_context_update_tool_runtime_context(&actor->context,
				lane->session.id);
		}
	}

	if (rc != 0) {
		runtime_close(actor);
		actor = NULL;
	}

	nb_mutex_lock(service->lock);
	lane->actor = actor;
	if (actor && service->stopping)
		react_cancel(actor->context.react);

	lane->actor_opening = 0;
	lane->last_used_ms = nb_time_monotonic_ms();
	nb_cond_broadcast(service->changed);
	nb_mutex_unlock(service->lock);
	return rc;
}

static int lane_prompt_pending(void *data)
{
	struct runtime_client_lane *lane = data;
	nb_mutex_lock(lane->service->lock);
	int pending = lane->turn && lane->turn->cancelled;
	nb_queue *entry;
	NB_QUEUE_FOREACH(entry, &lane->prompts) {
		struct runtime_client_prompt *prompt =
			NB_QUEUE_DATA(entry, struct runtime_client_prompt, node);

		if (prompt->expedited)
			pending = 1;
	}

	nb_mutex_unlock(lane->service->lock);
	return pending;
}

static int lane_action_drain(void *data, struct react_action *out, int timeout)
{
	struct runtime_client_lane *lane = data;
	(void)timeout;
	nb_mutex_lock(lane->service->lock);
	cJSON_free(lane->delivered_prompt);
	lane->delivered_prompt = NULL;
	if (lane->turn && lane->turn->cancelled && !lane->turn->cancel_delivered) {
		lane->turn->cancel_delivered = 1;
		out->type = "cancel";
		out->payload_json = "{}";
		nb_mutex_unlock(lane->service->lock);
		return 1;
	}

	if (!nb_queue_empty(&lane->prompts)) {
		struct runtime_client_prompt *prompt = NB_QUEUE_DATA(nb_queue_head(&lane->prompts),
			struct runtime_client_prompt, node);

		nb_queue_remove(&prompt->node);
		lane->delivered_prompt = prompt->payload;
		free(prompt);
	}

	out->type = "prompt";
	out->payload_json = lane->delivered_prompt;
	int available = out->payload_json != NULL;
	nb_mutex_unlock(lane->service->lock);
	return available;
}

static void lane_prompts_clear_locked(struct runtime_client_lane *lane)
{
	while (!nb_queue_empty(&lane->prompts)) {
		struct runtime_client_prompt *prompt = NB_QUEUE_DATA(nb_queue_head(&lane->prompts),
			struct runtime_client_prompt, node);

		nb_queue_remove(&prompt->node);
		cJSON_free(prompt->payload);
		free(prompt);
	}
}

static void lane_run_turn(struct runtime_client_lane *lane)
{
	struct runtime_client_service *service = lane->service;
	struct runtime_result result = {0};

	nb_mutex_lock(service->lock);
	struct runtime_client_turn *turn = lane->turn;
	int rc = turn->cancelled ? NB_ERR_CANCELLED : 0;

	if (rc == 0) {
		turn_status_set(turn, "running");
		(void)runtime_client_turn_store_locked(lane);
	}

	nb_mutex_unlock(service->lock);
	if (rc == 0) {
		(void)runtime_client_lane_emit(lane, "turn.started", "begin", NULL);
		rc = runtime_client_lane_ensure_actor(lane);
	}

	nb_mutex_lock(service->lock);
	if (rc == 0 && turn->cancelled)
		rc = NB_ERR_CANCELLED;

	nb_mutex_unlock(service->lock);
	if (rc == 0) {
		if (tool_borrow(&lane->actor->context.tools, "img_annotate"))
			(void)tool_unregister(&lane->actor->context.tools, "img_annotate");

		if (turn->terminal_handoff && service->owner->options.enable_img_annotate)
			rc = img_annotate_init_host(&lane->actor->context.tools,
				runtime_client_terminal_acquire, runtime_client_terminal_release,
				lane,
				lane->actor->context.tctx);
	}

	if (rc == 0) {
		struct runtime_request request = {
			.session_id = lane->session.id,
			.turn_id = turn->id,
			.model_input = turn->input,
			.stored_user_input = turn->stored_input,
			.user_id = turn->user_id,
			.generate_session_title = turn->generate_title,
			.turn_flags = turn->flags,
			.event_cb = runtime_client_lane_event,
			.event_user_data = lane,
			.hitl_cb = runtime_client_hitl,
			.hitl_user_data = lane,
			.override_hitl = 1,
			.ask_user_fn = runtime_client_question,
			.ask_user_user_data = lane,
			.override_ask_user = 1,
			.operation_approval_fn = runtime_client_approval,
			.operation_approval_user_data = lane,
			.override_operation_approval = 1,
			.prompt_pending_fn = lane_prompt_pending,
			.action_drain_fn = lane_action_drain,
			.action_drain_user_data = lane,
			.override_action_drain = 1,
			.mutation_begin = runtime_client_mutation_begin,
			.mutation_user_data = lane,
		};
		rc = runtime_execute_turn(lane->actor, &request, &result);
	}

	char *error_detail NB_CLEANUP(nb_charp_cleanup) =
		rc < 0 && lane->actor ? runtime_turn_error_message_copy(lane->actor, rc) : NULL;
	nb_mutex_lock(service->lock);
	turn->execution_rc = rc;
	turn->error_detail = error_detail;
	error_detail = NULL;
	turn->persistence_rc = result.persistence_rc;
	turn->final_text = strdup(result.final_text ? result.final_text : "");
	turn_status_set(turn, rc == 0 ? "completed" :
		(rc == NB_ERR_CANCELLED || rc == NB_ERR_INTERRUPTED || turn->cancelled)
		? "interrupted" : "failed");
	int stored = runtime_client_turn_store_locked(lane);
	if (stored != 0) {
		turn->persistence_rc = stored;
		turn_status_set(turn, "failed");
	}

	runtime_client_interactions_cancel_locked(lane);
	lane_prompts_clear_locked(lane);
	cJSON *terminal NB_CLEANUP(cJSON_DeleteOwner) = turn_snapshot_locked(lane);
	nb_mutex_unlock(service->lock);
	cJSON *completed NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (completed && terminal) {
		cJSON_AddStringToObject(completed, "type", "react");
		cJSON_AddStringToObject(completed, "name", "turn.completed");
		cJSON_AddStringToObject(completed, "phase", "end");
		cJSON_AddStringToObject(completed, "turn_id", turn->id);
		cJSON_AddItemToObject(completed, "data", terminal);
		terminal = NULL;
	}

	nb_mutex_lock(service->lock);
	if (completed)
		runtime_client_lane_publish_locked(lane, "turn.completed", completed);

	turn->active = 0;
	lane->working = 0;
	runtime_client_mutation_release_locked(lane);
	if (service->live_turns > 0)
		service->live_turns--;

	if (service->pending_turns > 0)
		service->pending_turns--;

	nb_cond_broadcast(service->changed);
	nb_mutex_unlock(service->lock);
}

static struct runtime_client_lane *lane_next_queued_locked(
	struct runtime_client_service *service)
{
	struct runtime_client_lane *next = NULL;

	for (size_t i = 0; i < service->lanes.len; i++) {
		struct runtime_client_lane **lane = nb_vec_at(&service->lanes, i);

		if ((*lane)->working && (*lane)->turn && (*lane)->turn->active &&
		    !(*lane)->turn->started && (!next || (*lane)->turn->order < next->turn->order))
			next = *lane;
	}

	return next;
}

static void *lane_worker_main(void *data)
{
	struct runtime_client_service *service = data;

	for (;;) {
		nb_mutex_lock(service->lock);
		struct runtime_client_lane *lane = lane_next_queued_locked(service);
		while (!lane && !service->stopping) {
			nb_cond_wait(service->changed, service->lock);
			lane = lane_next_queued_locked(service);
		}

		if (!lane) {
			nb_mutex_unlock(service->lock);
			break;
		}

		lane->turn->started = 1;
		service->live_turns++;
		nb_mutex_unlock(service->lock);
		lane_run_turn(lane);
	}

	return NULL;
}

int runtime_client_lane_create(struct runtime_client_service *service,
	int64_t session_id, struct runtime_client_lane **out)
{
	if (session_id <= 0)
		return NB_ERR_INVAL;

	nb_mutex_lock(service->lock);
	if (service->stopping) {
		nb_mutex_unlock(service->lock);
		return NB_ERR_CLOSED;
	}

	struct runtime_client_lane *existing = runtime_client_lane_find_locked(service, session_id);
	if (existing) {
		struct session current;
		int rc = session_get_by_id(&service->owner->context.database, session_id, &current);
		if (rc != 0) {
			nb_mutex_unlock(service->lock);
			return rc;
		}

		if (!existing->working && !existing->managing && !existing->controls)
			existing->session = current;

		*out = existing;
		nb_mutex_unlock(service->lock);
		return 0;
	}

	struct runtime_client_lane *lane = calloc(1, sizeof(*lane));
	if (!lane) {
		nb_mutex_unlock(service->lock);
		return NB_ERR_NOMEM;
	}

	lane->service = service;
	nb_queue_init(&lane->prompts);
	nb_queue_init(&lane->replay);
	nb_queue_init(&lane->interactions);
	nb_buf_init(&lane->stream_text);
	int rc = session_get_by_id(&service->owner->context.database, session_id, &lane->session);
	if (rc == 0)
		lane_settings_load(lane);

	struct runtime_client_lane **slot = rc == 0 ? nb_vec_push(&service->lanes) : NULL;
	if (rc == 0 && !slot)
		rc = NB_ERR_NOMEM;

	if (rc == 0) {
		*slot = lane;
		*out = lane;
	}

	if (rc != 0)
		lane->stop = 1;

	nb_cond_broadcast(service->changed);
	nb_mutex_unlock(service->lock);
	if (rc != 0) {
		runtime_client_lane_destroy(lane);
	}

	return rc;
}

void runtime_client_lane_destroy(struct runtime_client_lane *lane)
{
	if (!lane)
		return;

	runtime_close(lane->actor);
	lane_prompts_clear_locked(lane);
	while (!nb_queue_empty(&lane->replay)) {
		struct runtime_client_message_node *node = NB_QUEUE_DATA(
			nb_queue_head(&lane->replay),
			struct runtime_client_message_node, node);

		nb_queue_remove(&node->node);
		runtime_message_destroy(node->message);
		free(node);
	}

	runtime_client_turn_destroy(lane->turn);
	cJSON_free(lane->delivered_prompt);
	nb_buf_destroy(&lane->stream_text);
	free(lane);
}

static struct runtime_client_turn *turn_create_locked(struct runtime_client_lane *lane,
	struct runtime_client_request_node *request)
{
	struct runtime_client_turn *turn = calloc(1, sizeof(*turn));
	char id[96];

	if (!turn)
		return NULL;

	turn->order = ++lane->service->next_id;
	snprintf(id, sizeof(id), "turn_%s_%llu", lane->service->epoch,
		(unsigned long long)turn->order);
	const char *input = runtime_client_json_string(request->params, "input");
	if (!input[0])
		input = runtime_client_json_string(request->params, "model_input");

	const char *stored = runtime_client_json_string(request->params, "stored_user_input");
	turn->id = strdup(id);
	turn->input = strdup(input);
	turn->stored_input = strdup(stored[0] ? stored : input);
	turn->user_id = strdup(request->client->user_id);
	turn->idempotency_key =
		strdup(runtime_client_json_string(request->params, "idempotency_key"));
	turn->status = strdup("queued");
	turn->controller = request->client;
	turn->controller_capabilities = request->client->options.capabilities;
	turn->terminal_handoff = request->client->options.terminal_handoff;
	lane->interaction_controller = request->client;
	turn->active = 1;
	turn->generate_title = runtime_client_json_bool(request->params, "generate_session_title");
	const cJSON *flags = cJSON_GetObjectItemCaseSensitive(request->params, "turn_flags");
	double flags_value = cJSON_GetNumberValue(flags);
	turn->flags = cJSON_IsNumber(flags) && isfinite(flags_value) && flags_value >= 0 &&
		flags_value <= UINT32_MAX ? (unsigned)flags_value : AGENT_TURN_DEFAULT_FLAGS;
	if (!turn->id || !turn->input || !turn->stored_input || !turn->user_id ||
	    !turn->idempotency_key || !turn->status) {
		runtime_client_turn_destroy(turn);
		return NULL;
	}

	return turn;
}

static int turn_parameters_valid(const cJSON *params)
{
	const char *strings[] = {"input", "model_input", "stored_user_input",
		"idempotency_key", "turn_id", "display_text"};
	const char *booleans[] = {"generate_session_title", "preserve_processes", "expedite"};

	for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
		const cJSON *value = cJSON_GetObjectItemCaseSensitive(params, strings[i]);

		if (value && !cJSON_IsString(value))
			return 0;
	}

	for (size_t i = 0; i < sizeof(booleans) / sizeof(booleans[0]); i++) {
		const cJSON *value = cJSON_GetObjectItemCaseSensitive(params, booleans[i]);

		if (value && !cJSON_IsBool(value))
			return 0;
	}

	const cJSON *flags = cJSON_GetObjectItemCaseSensitive(params, "turn_flags");
	double number = cJSON_GetNumberValue(flags);
	if (flags && (!cJSON_IsNumber(flags) || !isfinite(number) || number < 0 ||
	    number > UINT32_MAX || floor(number) != number))
		return 0;

	return 1;
}

static int turn_start_locked(struct runtime_client_request_node *request,
	struct runtime_client_lane *lane, cJSON **result)
{
	const char *key = runtime_client_json_string(request->params, "idempotency_key");
	*result = turn_idempotency_copy(lane, request->client->user_id, key);
	if (*result) {
		(void)runtime_client_subscribe_locked(request->client, lane->session.id);
		return 0;
	}

	if (lane->working || lane->managing || lane->controls)
		return NB_ERR_BUSY;

	if (lane->service->pending_turns >= RUNTIME_CLIENT_REQUEST_LIMIT)
		return NB_ERR_BUSY;

	const cJSON *input = cJSON_GetObjectItemCaseSensitive(request->params, "input");
	const cJSON *model_input = cJSON_GetObjectItemCaseSensitive(request->params, "model_input");
	if (!cJSON_IsString(input) && !cJSON_IsString(model_input))
		return NB_ERR_INVAL;

	struct runtime_client_turn *turn = turn_create_locked(lane, request);
	if (!turn)
		return NB_ERR_NOMEM;

	while (lane->service->turn_worker_count < RUNTIME_CLIENT_TURN_LIMIT) {
		int index = lane->service->turn_worker_count;
		int worker_rc = nb_thread_create(&lane->service->turn_workers[index],
			lane_worker_main,
			lane->service, NULL);

		if (worker_rc != 0) {
			if (!lane->service->turn_worker_count) {
				runtime_client_turn_destroy(turn);
				return worker_rc;
			}

			break;
		}

		lane->service->turn_worker_count++;
	}

	int rc = runtime_client_subscribe_locked(request->client, lane->session.id);
	struct runtime_client_turn *previous = lane->turn;
	lane->turn = turn;
	if (rc == 0)
		rc = runtime_client_turn_store_locked(lane);

	if (rc != 0) {
		lane->turn = previous;
		runtime_client_turn_destroy(turn);
		return rc;
	}

	runtime_client_turn_destroy(previous);
	nb_buf_clear(&lane->stream_text);
	lane_prompts_clear_locked(lane);
	lane->working = 1;
	lane->service->pending_turns++;
	*result = turn_snapshot_locked(lane);
	return *result ? 0 : NB_ERR_NOMEM;
}

static int turn_steer_locked(struct runtime_client_request_node *request,
	struct runtime_client_lane *lane, cJSON **result)
{
	const char *text = runtime_client_json_string(request->params, "input");
	if (!text[0])
		return NB_ERR_INVAL;

	cJSON *payload NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	struct runtime_client_prompt *prompt = calloc(1, sizeof(*prompt));
	if (!payload || !prompt) {
		free(prompt);
		return NB_ERR_NOMEM;
	}

	cJSON_AddStringToObject(payload, "text", text);
	const char *display = runtime_client_json_string(request->params, "display_text");
	if (display[0])
		cJSON_AddStringToObject(payload, "display_text", display);

	prompt->payload = cJSON_PrintUnformatted(payload);
	prompt->expedited =
		!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(request->params, "expedite"));
	if (!prompt->payload) {
		free(prompt);
		return NB_ERR_NOMEM;
	}

	nb_queue_insert_tail(&lane->prompts, &prompt->node);
	*result = turn_snapshot_locked(lane);
	return *result ? 0 : NB_ERR_NOMEM;
}

int runtime_client_turn_command(struct runtime_client_request_node *request,
	struct runtime_client_lane *lane)
{
	struct runtime_client_service *service = lane->service;
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	struct runtime *cancel_actor = NULL;
	int preserve = 0;
	int rc = 0;
	const char *turn_id = runtime_client_json_string(request->params, "turn_id");

	nb_mutex_lock(service->lock);
	if (!turn_parameters_valid(request->params))
		rc = NB_ERR_INVAL;
	else if (!strcmp(request->method, "turn.start"))
		rc = turn_start_locked(request, lane, &result);
	else if (!strcmp(request->method, "turn.get")) {
		if (lane->turn && (!turn_id[0] || !strcmp(turn_id, lane->turn->id)))
			result = turn_snapshot_locked(lane);
		else
			result = runtime_client_turn_read(service, lane->session.id, turn_id);

		if (!result)
			rc = NB_ERR_NOT_FOUND;
	} else if (!strcmp(request->method, "turn.steer") ||
		   !strcmp(request->method, "turn.expedite") ||
		   !strcmp(request->method, "turn.interrupt")) {
		if (!turn_id[0])
			rc = NB_ERR_INVAL;
		else if (!lane->turn || strcmp(turn_id, lane->turn->id))
			rc = NB_ERR_NOT_FOUND;
		else if (strcmp(request->client->user_id, lane->turn->user_id))
			rc = NB_ERR_NOT_PERMITTED;
		else if (!strcmp(request->method, "turn.expedite")) {
			int applied = 0;
			nb_queue *entry;
			NB_QUEUE_FOREACH(entry, &lane->prompts) {
				struct runtime_client_prompt *prompt =
					NB_QUEUE_DATA(entry, struct runtime_client_prompt, node);

				prompt->expedited = 1;
				applied = 1;
			}

			result = turn_snapshot_locked(lane);
			if (result)
				cJSON_AddBoolToObject(result, "applied", applied);
		} else if (!strcmp(request->method, "turn.steer")) {
			if (!lane->working || lane->turn->cancelled)
				rc = NB_ERR_BUSY;
			else
				rc = turn_steer_locked(request, lane, &result);
		} else {
			if (lane->working) {
				lane->turn->cancelled = 1;
				preserve = runtime_client_json_bool(request->params,
					"preserve_processes");
				lane->turn->preserve_processes = preserve;
				turn_status_set(lane->turn, "cancelling");
				cancel_actor = lane->actor;
				if (cancel_actor)
					lane->controls++;

				runtime_client_interactions_cancel_locked(lane);
			}

			result = turn_snapshot_locked(lane);
		}
	} else {
		rc = NB_ERR_UNSUPPORTED;
	}

	(void)runtime_client_respond_locked(request, lane->session.id,
		lane->turn ? lane->turn->id : NULL, &result, rc);
	nb_cond_broadcast(service->changed);
	nb_mutex_unlock(service->lock);
	if (cancel_actor) {
		if (preserve)
			runtime_interrupt_background_wait(cancel_actor);
		else
			runtime_cancel_turn(cancel_actor);

		nb_mutex_lock(service->lock);
		lane->controls--;
		nb_cond_broadcast(service->changed);
		nb_mutex_unlock(service->lock);
	}

	return rc;
}
