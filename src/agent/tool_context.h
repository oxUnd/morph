#ifndef TOOL_CONTEXT_H
#define TOOL_CONTEXT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <stddef.h>

#define TOOL_CONTEXT_OUTPUT_DIR_MAX PATH_MAX
#define TOOL_CONTEXT_ALLOW_MAX 32
#define TOOL_CONTEXT_ALLOW_PATH_MAX PATH_MAX
#define TOOL_CONTEXT_ACTION_MAX 1024

enum tool_path_op {
	TOOL_PATH_READ = 0,
	TOOL_PATH_LIST = 1,
	TOOL_PATH_WRITE = 2,
};

enum tool_operation_kind {
	TOOL_OP_COMMAND = 0,
	TOOL_OP_PATH_READ = 1,
	TOOL_OP_PATH_LIST = 2,
	TOOL_OP_PATH_WRITE = 3,
	TOOL_OP_NETWORK = 4,
	TOOL_OP_EXTERNAL_SEND = 5,
};

enum tool_operation_verdict {
	TOOL_OP_DENY = 0,
	TOOL_OP_ALLOW = 1,
	TOOL_OP_ALWAYS = 2,
};

struct tool_operation {
	enum tool_operation_kind kind;
	const char *tool_name;
	const char *action;
	const char *target;
	const char *scope;
	const char *details_json;
};

typedef enum tool_operation_verdict (*tool_operation_approval_fn)(
	const struct tool_operation *op, void *user_data);

struct tool_context {
	char workdir[TOOL_CONTEXT_OUTPUT_DIR_MAX];
	char output_dir[TOOL_CONTEXT_OUTPUT_DIR_MAX];
	tool_operation_approval_fn operation_approval_fn;
	void *operation_approval_user_data;
	char read_allowed_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int read_allowed_dirs_count;
	char write_allowed_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int write_allowed_dirs_count;
	char allowed_commands[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ACTION_MAX];
	int allowed_commands_count;
	char exec_allowed_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int exec_allowed_dirs_count;
	int default_timeout_seconds;
};

struct tool_context *tool_context_create(const char *workdir,
					 const char *output_dir);
void tool_context_destroy(struct tool_context *tctx);
const char *tool_context_workdir(const struct tool_context *tctx);
const char *tool_context_output_dir(const struct tool_context *tctx);
void tool_context_set_default_timeout(struct tool_context *tctx, int seconds);
int tool_context_default_timeout(const struct tool_context *tctx);
int tool_context_authorize_path(struct tool_context *tctx,
				enum tool_path_op op, const char *path,
				char *resolved, size_t resolved_size);
int tool_context_check_operation(struct tool_context *tctx,
				 const struct tool_operation *op);
void tool_context_set_operation_approval(struct tool_context *tctx,
					 tool_operation_approval_fn fn,
					 void *user_data);
void tool_context_add_read_allowed_dir(struct tool_context *tctx,
				       const char *dir);
void tool_context_add_write_allowed_dir(struct tool_context *tctx,
					const char *dir);
int tool_context_allow_command_pattern(struct tool_context *tctx,
				       const char *pattern);
int tool_context_allow_command_scope(struct tool_context *tctx,
				     const char *path);

#ifdef __cplusplus
}
#endif

#endif
