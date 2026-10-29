#ifndef MORPH_EXEC_TOOL_H
#define MORPH_EXEC_TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include "event/event.h"

struct tool_context;
struct config_exec;
struct exec_jobs;
struct exec_jobs *exec_tool_jobs_handle(struct tool_registry *reg);

int exec_tool_init(struct tool_registry *reg, struct tool_context *tctx,
		   const struct config_exec *config);

int exec_tool_set_event_callback(struct tool_registry *reg, morph_event_cb cb,
				 void *user_data);
int exec_tool_jobs_list(struct tool_registry *reg, int64_t owner, cJSON **out);
int exec_tool_job_read(struct tool_registry *reg, int64_t owner, const char *id,
			cJSON **out);
int exec_tool_job_stop(struct tool_registry *reg, int64_t owner, const char *id);

#ifdef __cplusplus
}
#endif

#endif
