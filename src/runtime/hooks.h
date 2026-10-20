#ifndef MORPH_RUNTIME_HOOKS_H
#define MORPH_RUNTIME_HOOKS_H

#include "agent/hooks.h"
#include "config/config.h"
#include "db/database.h"
#include "event/event.h"
#include "util/array.h"
#include "util/cancel.h"

#ifdef __cplusplus
extern "C" {
#endif

struct hook_manager {
	struct db *db;
	const struct config *config;
	char config_path[PATH_MAX];
	char workdir[PATH_MAX];
	morph_array_t entries;
	int ready;
	int enabled;
	cJSON *recent;
	size_t context_bytes;
	uint64_t remaining_ms;
	char *session_context;
	int64_t session_id;
	const char *turn_id;
	const char *model;
	morph_event_cb event_cb;
	void *event_user;
	const volatile sig_atomic_t *cancelled;
	struct morph_cancel_token *cancel_token;
};

int hook_manager_init(struct hook_manager *manager, struct db *db, const struct config *config,
		      const char *config_path, const char *workdir);
void hook_manager_cleanup(struct hook_manager *manager);
int hook_manager_reload(struct hook_manager *manager);
cJSON *hook_manager_list(struct hook_manager *manager);
int hook_manager_action(struct hook_manager *manager, const char *action, const char *id,
			const char *expected_digest);
int hook_manager_dispatch(void *manager, const char *event, const cJSON *input, cJSON **output);
int hook_manager_has(struct hook_manager *manager, const char *event);

#ifdef __cplusplus
}
#endif
#endif
