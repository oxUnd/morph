#ifndef MORPH_RUNTIME_BOOTSTRAP_H
#define MORPH_RUNTIME_BOOTSTRAP_H

#include "agent/react.h"
#include "agent/agent_control.h"
#include "agent/tool_context.h"
#include "agent/tools/img_annotate.h"
#include "agent/tools/scheduled_tasks.h"
#include "config/config.h"
#include "db/database.h"
#include "event/event.h"
#include "models/llm.h"
#include "runtime/runtime.h"
#include "skill/skill.h"
#include <limits.h>

struct nb_log;
struct exec_tool_jobs_handle;

struct runtime_models {
	struct model *memory_text;
	struct tokenizer *tokenizer;
	struct react_context *react;
	struct model *text;
	struct model *vision;
	struct model *image;
	struct model *video;
};

struct runtime_bootstrap_profile {
	struct runtime *owner;
	struct nb_log *logger;
	struct config *config;
	struct db *db;
	struct tool_registry *tools;
	struct tool_context **tool_context;
	struct skill_registry **skills;
	struct plan_registry *plans;
	struct runtime_models *models;
	const char *workdir;
	const char *config_path;
	morph_event_cb event_cb;
	void *event_user_data;
	struct model_usage_sink *usage_sink;
	runtime_hitl_approval_fn hitl_cb;
	void *hitl_user_data;
	runtime_ask_user_fn ask_user_cb;
	void *ask_user_user_data;
	runtime_operation_approval_fn operation_approval_cb;
	void *operation_approval_user_data;
	struct scheduled_task_event_sink *task_events;
	runtime_platform_tools_fn platform_tools_cb;
	void *platform_tools_user_data;
	img_annotate_pause_fn img_annotate_pause_cb;
	img_annotate_resume_fn img_annotate_resume_cb;
	void *img_annotate_user_data;
	int enable_exec;
	int enable_apply_patch;
	int enable_config_write;
	int enable_img_annotate;
	int enable_shell_exts;
	int enable_multi_agent;
	int allocate_skill_registry;
	int process_replica;
};

struct runtime_shutdown_resources {
	struct memory_worker **memory_worker;
	struct db *db;
	struct tool_registry *tools;
	struct exec_tool_jobs_handle *jobs_owner;
	struct tool_context **tool_context;
	struct skill_registry **skills;
	struct mcp_registry *mcp;
	struct agent_control **agents;
	struct react_context **react;
	struct tokenizer **tokenizer;
	struct model **text;
	struct model **memory_text;
	struct model **vision;
	struct model **image;
	struct model **video;
	int free_skill_registry;
};

int runtime_bootstrap_models(struct runtime_bootstrap_profile *profile);
int runtime_bootstrap_tools(struct runtime_bootstrap_profile *profile);
void runtime_bootstrap_tool_flags(struct tool_registry *tools, const struct config *config);
int runtime_bootstrap_dynamic_tools(struct runtime_bootstrap_profile *profile,
				    const char *session_id);
int runtime_bootstrap_child(struct tool_registry *tools, struct plan_registry *plans,
	struct react_context *react, struct model *model, const struct tool_runtime_context *runtime,
	struct agent_child_state *owner, void *user_data);
int runtime_bootstrap_agents(struct runtime_bootstrap_profile *profile,
				 struct agent_control **out);
void runtime_bootstrap_cleanup_models(struct runtime_models *models);
void runtime_bootstrap_cleanup(struct runtime_shutdown_resources *resources);

#endif
