#ifndef MORPH_RUNTIME_HOOKS_H
#define MORPH_RUNTIME_HOOKS_H

struct nb_log;
#include "agent/hooks.h"
#include "nb/thread.h"
#include "config/config.h"
#include "db/database.h"
#include "event/event.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "nb/cancel.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tool_context;

struct hook_manager {
	struct tool_context *permissions;
	struct nb_log *logger;
	struct db *db;
	const struct config *config;
	char config_path[PATH_MAX];
	char workdir[PATH_MAX];
	nb_vec entries;
	int ready;
	int enabled;
	struct config_file_error error;
	cJSON *recent;
	size_t context_bytes;
	uint64_t remaining_ms;
	char *session_context;
	int64_t session_id;
	const char *turn_id;
	const char *model;
	morph_event_cb event_cb;
	void *event_user;
	const nb_atomic_int *cancelled;
	struct nb_cancel *cancel_token;
};

int hook_manager_init(struct hook_manager *manager, struct db *db, const struct config *config,
		      const char *config_path, const char *workdir);
void hook_manager_cleanup(struct hook_manager *manager);
int hook_manager_reload(struct hook_manager *manager);
/* Returns a complete independent JSON owner, or NULL without a partial result. */
cJSON *hook_manager_list(struct hook_manager *manager);
int hook_manager_action(struct hook_manager *manager, const char *action, const char *id,
			const char *expected_digest);
/* An empty output receives an independent JSON owner only on success. */
int hook_manager_dispatch(void *manager, const char *event, const cJSON *input, cJSON **output);
int hook_manager_has(struct hook_manager *manager, const char *event);

#ifdef __cplusplus
}
#endif
#endif
