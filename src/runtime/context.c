#include "runtime/context.h"

#include "util/error.h"

#include <errno.h>
#include <string.h>

static int runtime_plan_session_slot(struct runtime_plan_session *sessions,
				     int session_count,
				     int64_t session_id)
{
	if (!sessions || session_count <= 0 || session_id <= 0)
		return -1;
	for (int i = 0; i < session_count; i++) {
		if (sessions[i].session_id == session_id)
			return i;
	}
	return -1;
}

/*
 * Monotonic access counter used for LRU eviction. Returns a value strictly
 * greater than every occupied slot's last_used, so the most recently touched
 * slot always holds the largest stamp. Callers hold the runtime execution
 * lock, so no additional synchronization is required.
 */
static uint64_t runtime_plan_session_next_stamp(
	struct runtime_plan_session *sessions, int session_count)
{
	uint64_t max = 0;

	for (int i = 0; i < session_count; i++) {
		if (sessions[i].session_id != 0 &&
		    sessions[i].last_used > max)
			max = sessions[i].last_used;
	}
	return max + 1;
}

static int runtime_plan_session_alloc_slot(
	struct runtime_plan_session *sessions, int session_count,
	int64_t active_session_id, int64_t session_id)
{
	int lru = -1;

	if (!sessions || session_count <= 0 || session_id <= 0)
		return -1;
	for (int i = 0; i < session_count; i++) {
		if (sessions[i].session_id == 0) {
			sessions[i].session_id = session_id;
			sessions[i].last_used = runtime_plan_session_next_stamp(
				sessions, session_count);
			plan_registry_init(&sessions[i].registry);
			return i;
		}
		if (sessions[i].session_id == active_session_id)
			continue;
		if (lru < 0 ||
		    sessions[i].last_used < sessions[lru].last_used)
			lru = i;
	}
	if (lru < 0)
		lru = 0;
	sessions[lru].session_id = session_id;
	sessions[lru].last_used = runtime_plan_session_next_stamp(
		sessions, session_count);
	plan_registry_init(&sessions[lru].registry);
	return lru;
}

static void runtime_plan_session_save_active(
	struct runtime_plan_session *sessions, int session_count,
	int64_t active_session_id, const struct plan_registry *active_plans)
{
	int slot;

	if (!sessions || !active_plans || active_session_id <= 0)
		return;
	slot = runtime_plan_session_slot(sessions, session_count,
					 active_session_id);
	if (slot < 0)
		slot = runtime_plan_session_alloc_slot(
			sessions, session_count, active_session_id,
			active_session_id);
	if (slot >= 0)
		sessions[slot].registry = *active_plans;
}

void runtime_plan_session_select(struct runtime_plan_session *sessions,
				 int session_count,
				 int64_t *active_session_id,
				 struct plan_registry *active_plans,
				 int64_t session_id)
{
	int slot;

	if (!sessions || !active_session_id || !active_plans)
		return;
	if (session_id <= 0) {
		plan_registry_init(active_plans);
		*active_session_id = 0;
		return;
	}
	if (*active_session_id == session_id)
		return;

	runtime_plan_session_save_active(sessions, session_count,
					 *active_session_id, active_plans);
	slot = runtime_plan_session_slot(sessions, session_count, session_id);
	if (slot >= 0) {
		*active_plans = sessions[slot].registry;
		sessions[slot].last_used = runtime_plan_session_next_stamp(
			sessions, session_count);
	} else {
		plan_registry_init(active_plans);
		slot = runtime_plan_session_alloc_slot(
			sessions, session_count, *active_session_id,
			session_id);
		if (slot >= 0)
			sessions[slot].registry = *active_plans;
	}
	*active_session_id = session_id;
}

void runtime_plan_session_forget(struct runtime_plan_session *sessions,
				 int session_count,
				 int64_t *active_session_id,
				 struct plan_registry *active_plans,
				 int64_t session_id)
{
	int slot;

	if (!sessions || !active_session_id || !active_plans ||
	    session_id <= 0)
		return;
	if (*active_session_id == session_id) {
		plan_registry_init(active_plans);
		*active_session_id = 0;
	}
	slot = runtime_plan_session_slot(sessions, session_count, session_id);
	if (slot >= 0) {
		sessions[slot].session_id = 0;
		sessions[slot].last_used = 0;
		plan_registry_init(&sessions[slot].registry);
	}
}

int runtime_tool_context_for_session(struct db *db,
				     const struct config *config,
				     const struct session *current_session,
				     int64_t session_id,
				     struct tool_runtime_context *out)
{
	struct session s;
	const char *credit_session_id = NULL;

	if (!db || !config || !out)
		MORPH_RETURN(-EINVAL);
	memset(out, 0, sizeof(*out));
	out->db = db;
	out->config = config;
	out->user_id = "local";
	if (current_session && session_id == current_session->id) {
		credit_session_id = current_session->display_id[0]
			? current_session->display_id
			: current_session->name;
	} else if (session_id > 0 &&
		   session_get_by_id(db, session_id, &s) == 0) {
		session_ensure_display_id(db, &s);
		credit_session_id = s.display_id[0] ? s.display_id : s.name;
	}
	out->credit_session_id = credit_session_id ? credit_session_id : "local";
	out->memory_session_id = session_id;
	out->restrict_memory_to_user = 0;
	return 0;
}
