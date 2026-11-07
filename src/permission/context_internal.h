#ifndef MORPH_PERMISSION_CONTEXT_INTERNAL_H
#define MORPH_PERMISSION_CONTEXT_INTERNAL_H

#include "context.h"

struct tool_call_grant {
	char principal[TOOL_CONTEXT_ACTION_MAX];
	int host;
	int network;
};

struct tool_context {
	pthread_mutex_t authority_lock;
	struct permission_context permissions;
	struct tool_call_grant tool_calls[TOOL_CONTEXT_ALLOW_MAX];
	int tool_calls_count;
	struct morph_log *logger;
	char workdir[TOOL_CONTEXT_OUTPUT_DIR_MAX];
	char output_dir[TOOL_CONTEXT_OUTPUT_DIR_MAX];
	tool_operation_approval_fn operation_approval_fn;
	void *operation_approval_user_data;
	char read_allowed_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int read_allowed_dirs_count;
	char write_allowed_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int write_allowed_dirs_count;
	char delete_allowed_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int delete_allowed_dirs_count;
	char allowed_commands[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ACTION_MAX];
	int allowed_commands_count;
	char exec_allowed_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int exec_allowed_dirs_count;
	morph_array_t builtin_paths;
	morph_array_t scoped_grants;
	morph_array_t persistent_grants;
	struct db *grant_db;
	char grant_project_root[TOOL_CONTEXT_ALLOW_PATH_MAX];
	int default_timeout_seconds;
	char exec_profile_write_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int exec_profile_write_dirs_count;
	char exec_profile_delete_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int exec_profile_delete_dirs_count;
};

#endif
