#ifndef MORPH_RUNTIME_H
#define MORPH_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/react.h"
#include "agent/history.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "config/config.h"
#include "event/event.h"
#include "models/llm.h"
#include "mcp/mcp.h"
#include "runtime/request.h"
#include "runtime/result.h"
#include "runtime/tasks.h"
#include "runtime/sync.h"
#include "runtime/services.h"
#include "agent/tools/img_annotate.h"
#include "agent/sub_agent.h"
#include "session.h"
#include <stdint.h>

/*
 * Public runtime boundary. A front owns a runtime handle, never the database,
 * agent, model, or tool registry behind it. Platform callbacks are injected at
 * creation time so the same runtime can serve CLI, Android, or another host.
 */
struct runtime;

typedef enum hitl_verdict (*runtime_hitl_approval_fn)(
	const char *tool_name, const char *tool_args, void *user_data);
typedef int (*runtime_ask_user_fn)(const char *question,
	const char *const *choices, int choices_count,
	const char *selection_mode, int min_choices, int max_choices,
	char ***answers, int *answers_count, void *user_data);
typedef enum tool_operation_verdict (*runtime_operation_approval_fn)(
	const struct tool_operation *op, void *user_data);
typedef int (*runtime_platform_tools_fn)(struct tool_registry *tools,
	struct tool_context *tctx, void *user_data);
typedef void (*runtime_usage_observer_fn)(const struct model_usage *usage,
	void *user_data);
typedef int (*runtime_after_models_fn)(struct react_context *react,
	void *user_data);
struct runtime_options {
	const char *config_path;
	const char *db_path;
	const char *workdir;
	const char *output_dir_override;
	const char *default_dynamic_tools_mode;
	const char *default_session;
	const char *front_name;
	morph_event_cb event_cb;
	void *event_user_data;
	runtime_usage_observer_fn usage_observer;
	void *usage_observer_user_data;
	runtime_hitl_approval_fn hitl_cb;
	void *hitl_user_data;
	runtime_ask_user_fn ask_user_cb;
	void *ask_user_user_data;
	runtime_operation_approval_fn operation_approval_cb;
	void *operation_approval_user_data;
	runtime_platform_tools_fn platform_tools_cb;
	void *platform_tools_user_data;
	runtime_after_models_fn after_models_cb;
	void *after_models_user_data;
	struct scheduled_task_event_sink *task_events;
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
	int auto_connect_mcp;
	int create_new_session;
	int restore_recent_session;
	int process_replica;
};

struct runtime_permission_grant {
	int64_t id;
	char subject[256];
	char resource_kind[32];
	char resource[PATH_MAX];
	char project_root[PATH_MAX];
	int64_t created_at;
};

int runtime_open(const struct runtime_options *options,
		 struct runtime **out);
void runtime_close(struct runtime *runtime);

int runtime_execute_turn(struct runtime *runtime,
			 const struct runtime_request *request,
			 struct runtime_result *result);
void runtime_cancel_turn(struct runtime *runtime);

int runtime_session_current(const struct runtime *runtime,
			    struct session *out);
int runtime_session_current_id(const struct runtime *runtime,
			       int64_t *out);
int runtime_session_select(struct runtime *runtime, const char *name,
			   struct session *out, int *created);
int runtime_session_create_and_select(struct runtime *runtime,
				      const char *name, struct session *out);
int runtime_session_create_detached(struct runtime *runtime, const char *name,
			   struct session *out);
int runtime_session_delete_and_update(struct runtime *runtime, int64_t id);
int runtime_session_rename_and_update(struct runtime *runtime, int64_t id,
				      const char *name);
int runtime_session_list_all(struct runtime *runtime, struct session **out,
			     int *count, int recent_first);
void runtime_session_list_free(struct session *sessions);
int runtime_session_reload_current(struct runtime *runtime);
const char *runtime_session_current_name(const struct runtime *runtime);
struct message *runtime_session_messages_current(struct runtime *runtime,
					 int *count);
void runtime_session_messages_free(struct message *messages);
struct model_history_item *runtime_session_model_history_current(
	struct runtime *runtime, int active_only, int *count);
void runtime_session_model_history_free(struct model_history_item *items);
int runtime_session_history_diagnose(struct runtime *runtime,
	struct agent_history_diagnostic *diagnostic);
int runtime_session_history_repair(struct runtime *runtime,
	struct agent_history_diagnostic *before, int *changed);

int runtime_register_tool(struct runtime *runtime,
			  const struct tool_spec *spec);
int runtime_add_mcp_server(struct runtime *runtime,
			   const struct mcp_server_config *server);
const struct config *runtime_config_get(const struct runtime *runtime);
const char *runtime_workdir_get(const struct runtime *runtime);
const char *runtime_config_path_get(const struct runtime *runtime);
char *runtime_output_get_json(struct runtime *runtime, const char *path);
int runtime_permission_list(struct runtime *runtime,
			    struct runtime_permission_grant **out,
			    int *count);
void runtime_permission_list_free(struct runtime_permission_grant *grants);
int runtime_permission_revoke_id(struct runtime *runtime, int64_t id,
				 int *deleted);
int runtime_permission_revoke_subject(struct runtime *runtime,
				      const char *subject, int *deleted);
int runtime_permission_clear(struct runtime *runtime, int all_projects,
				     int *deleted);

int runtime_sub_agent_list(struct runtime *runtime,
			   int64_t parent_session_id,
			   struct sub_agent_task_info **out, int *count);
void runtime_sub_agent_list_free(struct sub_agent_task_info *tasks,
				 int count);
int runtime_sub_agent_select(struct runtime *runtime, const char *task_id);
int runtime_sub_agent_events(struct runtime *runtime, const char *task_id,
			     char ***events, int *count);
void runtime_sub_agent_events_free(char **events, int count);


#ifdef __cplusplus
}
#endif

#endif
