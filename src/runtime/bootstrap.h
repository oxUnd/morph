#ifndef MORPH_RUNTIME_BOOTSTRAP_H
#define MORPH_RUNTIME_BOOTSTRAP_H

#include "agent/react.h"
#include "agent/sub_agent.h"
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

struct runtime_models {
	struct tokenizer *tokenizer;
	struct react_context *react;
	struct model *text;
	struct model *vision;
	struct model *image;
	struct model *video;
};

struct runtime_bootstrap_profile {
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
	model_usage_callback usage_cb;
	void *usage_user_data;
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
	int enable_bash;
	int enable_apply_patch;
	int enable_config_write;
	int enable_img_annotate;
	int enable_shell_exts;
	int enable_sub_agents;
	int allocate_skill_registry;
};

struct runtime_shutdown_resources {
	struct db *db;
	struct tool_registry *tools;
	struct tool_context **tool_context;
	struct skill_registry **skills;
	struct mcp_registry *mcp;
	struct sub_agent_runtime **sub_agents;
	struct react_context **react;
	struct tokenizer **tokenizer;
	struct model **text;
	struct model **vision;
	struct model **image;
	struct model **video;
	int shutdown_memory;
	int reset_usage_callbacks;
	int free_skill_registry;
};

int runtime_bootstrap_models(struct runtime_bootstrap_profile *profile);
int runtime_bootstrap_tools(struct runtime_bootstrap_profile *profile);
int runtime_bootstrap_dynamic_tools(struct runtime_bootstrap_profile *profile,
				    const char *session_id);
int runtime_bootstrap_sub_agents(struct runtime_bootstrap_profile *profile,
				 struct sub_agent_runtime **out);
void runtime_bootstrap_cleanup_models(struct runtime_models *models);
void runtime_bootstrap_cleanup(struct runtime_shutdown_resources *resources);

#endif
