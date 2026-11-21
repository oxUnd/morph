#include "runtime/client_internal.h"

#include "runtime/runtime_internal.h"
#include "agent/tools/exec_jobs.h"
#include <string.h>

static const char *lane_workspace(const struct runtime_client_lane *lane)
{
	if (lane->session.workdir[0])
		return lane->session.workdir;

	return lane->service->owner->context.workdir;
}

static int workspace_contains(const char *parent, const char *child)
{
	size_t length = strlen(parent);

	while (length > 1 && parent[length - 1] == '/')
		length--;

	return !strncmp(parent, child, length) &&
		(child[length] == '\0' || child[length] == '/' ||
		 (length == 1 && parent[0] == '/'));
}

static int workspace_overlaps(const struct runtime_client_lane *left,
	const struct runtime_client_lane *right)
{
	const char *a = lane_workspace(left);
	const char *b = lane_workspace(right);

	return workspace_contains(a, b) || workspace_contains(b, a);
}

int runtime_client_mutation_begin(void *data, const char *tool)
{
	struct runtime_client_lane *lane = data;
	struct runtime_client_service *service = lane->service;
	const char *const state_tools[] = {
		"credits", "memory", "memory_preference", "memory_update", "plan", "update_plan",
		"activate_skill", "ask_user", "request_permissions", "tasks", "spawn_agent",
		"send_message", "followup_task", "interrupt_agent", "list_agents", "read_agent",
		"wait_agent", "background"
	};

	for (size_t i = 0; i < sizeof(state_tools) / sizeof(state_tools[0]); i++) {
		if (tool && !strcmp(tool, state_tools[i]))
			return 0;
	}

	nb_mutex_lock(service->lock);
	for (;;) {
		if (service->stopping || (lane->working && lane->turn && lane->turn->cancelled)) {
			nb_mutex_unlock(service->lock);
			return NB_ERR_CANCELLED;
		}

		int blocked = 0;
		for (size_t i = 0; i < service->lanes.len; i++) {
			struct runtime_client_lane **other = nb_vec_at(&service->lanes, i);

			if (*other != lane && (*other)->mutation_claimed &&
			    workspace_overlaps(lane, *other)) {
				blocked = 1;
				break;
			}
		}

		if (!blocked) {
			lane->mutation_claimed = 1;
			nb_mutex_unlock(service->lock);
			return 0;
		}

		(void)nb_cond_timedwait(service->changed, service->lock, INT64_C(100000000));
	}
}

void runtime_client_mutation_release_locked(struct runtime_client_lane *lane)
{
	if (lane->actor && exec_jobs_active(lane->actor->jobs))
		return;

	if (lane->actor && agent_control_active(lane->actor->context.agents, 0))
		return;

	lane->mutation_claimed = 0;
	nb_cond_broadcast(lane->service->changed);
}
