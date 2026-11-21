#ifndef MORPH_RUNTIME_CLIENT_INTERNAL_H
#define MORPH_RUNTIME_CLIENT_INTERNAL_H

#include "runtime/client.h"
#include "runtime/runtime.h"
#include <nb/buf.h>
#include <nb/map.h>
#include <nb/queue.h>
#include <nb/thread.h>
#include <nb/vec.h>

#define RUNTIME_CLIENT_REQUEST_LIMIT 128
#define RUNTIME_CLIENT_EVENT_LIMIT 512
#define RUNTIME_CLIENT_REPLAY_LIMIT 256
#define RUNTIME_CLIENT_SESSION_LIMIT 64
#define RUNTIME_CLIENT_TURN_LIMIT 8
#define RUNTIME_CLIENT_MANAGEMENT_LIMIT 4

struct runtime_client_service;
struct runtime_client_lane;

struct runtime_client_message_node {
	nb_queue node;
	struct runtime_message *message;
};

struct runtime_client_request_node {
	nb_queue node;
	struct runtime_client *client;
	char *id;
	char *method;
	cJSON *params;
	int retained;
};

struct runtime_client_subscription {
	int64_t session_id;
	int suspended;
};

struct runtime_client {
	struct runtime_client_service *service;
	nb_queue node;
	nb_queue messages;
	nb_vec subscriptions;
	nb_map *requests;
	struct runtime_client_options options;
	char *user_id;
	size_t message_events;
	size_t pending_requests;
	int closed;
	int released;
	size_t references;
};

struct runtime_client_turn {
	char *id;
	char *idempotency_key;
	uint64_t order;
	char *input;
	char *stored_input;
	char *user_id;
	unsigned flags;
	int generate_title;
	int cancelled;
	int cancel_delivered;
	int preserve_processes;
	int active;
	int started;
	int execution_rc;
	int persistence_rc;
	char *status;
	char *final_text;
	char *error_detail;
	struct runtime_client *controller;
	unsigned controller_capabilities;
	int terminal_handoff;
};

struct runtime_client_prompt {
	nb_queue node;
	char *payload;
	int expedited;
};

struct runtime_client_interaction {
	nb_queue node;
	char *id;
	char *turn_id;
	char *kind;
	char *user_id;
	cJSON *request;
	cJSON *reply;
	struct runtime_client *controller;
	struct runtime_client_lane *lane;
	int error_code;
	int resolved;
	int deadline_disabled;
};

struct runtime_client_lane {
	struct runtime_client_service *service;
	struct session session;
	struct runtime *actor;
	nb_queue prompts;
	nb_queue replay;
	nb_queue interactions;
	size_t replay_count;
	uint64_t sequence;
	struct runtime_client_turn *turn;
	char *delivered_prompt;
	nb_buf stream_text;
	int working;
	int stop;
	int actor_opening;
	int64_t last_used_ms;
	int managing;
	size_t controls;
	struct permission_policy permission;
	int has_permission;
	int ultra;
	int has_ultra;
	int mutation_claimed;
	struct runtime_client *management_controller;
	struct runtime_client *interaction_controller;
};

struct runtime_client_service {
	struct runtime *owner;
	nb_mutex *lock;
	nb_mutex *domain_lock;
	nb_cond *changed;
	nb_thread *dispatcher;
	nb_thread *management_workers[RUNTIME_CLIENT_MANAGEMENT_LIMIT];
	int management_worker_count;
	nb_thread *turn_workers[RUNTIME_CLIENT_TURN_LIMIT];
	int turn_worker_count;
	nb_queue clients;
	nb_queue requests;
	nb_queue management_requests;
	nb_vec lanes;
	uint64_t next_id;
	char epoch[65];
	size_t live_turns;
	size_t pending_turns;
	int stopping;
	int owner_released;
	int destroying;
	size_t receivers;
	size_t direct_dispatches;
	int storage_ready;
};

int runtime_clients_init(struct runtime *runtime);
void runtime_clients_shutdown(struct runtime *runtime);
int runtime_client_service_interrupted(void *data);

struct runtime_message *runtime_client_message_create(enum runtime_message_kind kind,
	const char *id, const char *method, int64_t session_id, const char *turn_id,
	uint64_t sequence, const cJSON *data, int error_code);
/* All *_locked helpers require service->lock. They never borrow payloads. */
int runtime_client_enqueue_locked(struct runtime_client *client,
	struct runtime_message **message);
int runtime_client_respond_locked(struct runtime_client_request_node *request,
	int64_t session_id, const char *turn_id, cJSON **data, int error_code);
int runtime_client_subscribe_locked(struct runtime_client *client, int64_t session_id);
int runtime_client_is_subscribed_locked(struct runtime_client *client, int64_t session_id);
struct runtime_client_lane *runtime_client_lane_find_locked(
	struct runtime_client_service *service, int64_t session_id);
int runtime_client_lane_create(struct runtime_client_service *service,
	int64_t session_id, struct runtime_client_lane **out);
int runtime_client_lane_ensure_actor(struct runtime_client_lane *lane);
void runtime_client_lane_destroy(struct runtime_client_lane *lane);
void runtime_client_turn_destroy(struct runtime_client_turn *turn);
cJSON *runtime_client_lane_snapshot_locked(struct runtime_client_lane *lane);
int runtime_client_turn_command(struct runtime_client_request_node *request,
	struct runtime_client_lane *lane);
void runtime_client_lane_publish_locked(struct runtime_client_lane *lane,
	const char *name, const cJSON *event);
int runtime_client_lane_event(const struct morph_event *event, void *user_data);
int runtime_client_lane_emit(struct runtime_client_lane *lane,
	const char *name, const char *phase, const cJSON *data);
int runtime_client_interaction_request(struct runtime_client_lane *lane,
	const char *kind, const cJSON *request, const struct nb_cancel *cancel,
	int timeout_seconds, cJSON **out);
enum hitl_verdict runtime_client_hitl(const char *tool, const char *args, void *data);
enum tool_operation_verdict runtime_client_approval(
	const struct tool_operation *operation, void *data);
int runtime_client_question(const char *question, const char *const *choices,
	int choices_count, const char *selection_mode, int min_choices, int max_choices,
	const struct ask_user_options *options, char ***answers, int *answers_count,
	void *data);
void runtime_client_interactions_cancel_locked(struct runtime_client_lane *lane);
int runtime_client_turn_storage_init(struct runtime_client_service *service);
void runtime_client_turn_storage_close(struct runtime_client_service *service);
int runtime_client_lane_settings_store(struct runtime_client_lane *lane);
int runtime_client_claim_interactions(struct runtime_client_request_node *request,
	struct runtime_client_lane *lane);
int runtime_client_interaction_progress(struct runtime_client_request_node *request);
int runtime_client_mutation_begin(void *data, const char *tool);
void runtime_client_mutation_release_locked(struct runtime_client_lane *lane);
int runtime_client_terminal_acquire(void *data);
void runtime_client_terminal_release(void *data);
int runtime_client_turn_store_locked(struct runtime_client_lane *lane);
cJSON *runtime_client_turn_read(struct runtime_client_service *service,
	int64_t session_id, const char *turn_id);

const char *runtime_client_json_string(const cJSON *object, const char *name);
int64_t runtime_client_json_id(const cJSON *object, const char *name);
int runtime_client_json_bool(const cJSON *object, const char *name);

#endif
