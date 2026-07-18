#ifndef MORPH_RUNTIME_CONTEXT_H
#define MORPH_RUNTIME_CONTEXT_H

#include "agent/plan.h"
#include "agent/tool_runtime.h"
#include "db/database.h"
#include "../session.h"

#include <stdint.h>

struct runtime_plan_session {
	int64_t session_id;
	struct plan_registry registry;
};

void runtime_plan_session_select(struct runtime_plan_session *sessions,
				 int session_count,
				 int64_t *active_session_id,
				 struct plan_registry *active_plans,
				 int64_t session_id);
void runtime_plan_session_forget(struct runtime_plan_session *sessions,
				 int session_count,
				 int64_t *active_session_id,
				 struct plan_registry *active_plans,
				 int64_t session_id);

int runtime_tool_context_for_session(struct db *db,
				     const struct config *config,
				     const struct session *current_session,
				     int64_t session_id,
				     struct tool_runtime_context *out);

#endif
