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
#define TOOL_CONTEXT_COMMAND_MAX 1024

enum write_verdict {
	WRITE_DENY = 0,
	WRITE_ALLOW = 1,
	WRITE_ALWAYS = 2,
};

enum tool_path_op {
	TOOL_PATH_READ = 0,
	TOOL_PATH_LIST = 1,
	TOOL_PATH_WRITE = 2,
};

enum tool_path_verdict {
	TOOL_PATH_DENY = 0,
	TOOL_PATH_ALLOW = 1,
	TOOL_PATH_ALWAYS = 2,
};

typedef enum write_verdict (*tool_write_approval_fn)(const char *path,
						     const char *output_dir,
						     void *user_data);

typedef enum tool_path_verdict (*tool_path_approval_fn)(
	enum tool_path_op op, const char *path, const char *root,
	void *user_data);

enum command_verdict {
	COMMAND_DENY = 0,
	COMMAND_ALLOW = 1,
	COMMAND_ALWAYS = 2,
};

typedef enum command_verdict (*tool_command_approval_fn)(
	const char *command, const char *cwd, void *user_data);

struct tool_context {
	char workdir[TOOL_CONTEXT_OUTPUT_DIR_MAX];
	char output_dir[TOOL_CONTEXT_OUTPUT_DIR_MAX];
	tool_path_approval_fn path_approval_fn;
	void *path_approval_user_data;
	tool_write_approval_fn approval_fn;
	void *approval_user_data;
	char read_allowed_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int read_allowed_dirs_count;
	char allowed_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int allowed_dirs_count;
	tool_command_approval_fn command_approval_fn;
	void *command_approval_user_data;
	char allowed_commands[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_COMMAND_MAX];
	int allowed_commands_count;
	char exec_allowed_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int exec_allowed_dirs_count;
};

struct tool_context *tool_context_create(const char *workdir,
					 const char *output_dir);
void tool_context_destroy(struct tool_context *tctx);
const char *tool_context_workdir(const struct tool_context *tctx);
const char *tool_context_output_dir(const struct tool_context *tctx);
int tool_context_authorize_path(struct tool_context *tctx,
				enum tool_path_op op, const char *path,
				char *resolved, size_t resolved_size);
int tool_context_check_read_path(struct tool_context *tctx, const char *path);
int tool_context_check_write_path(struct tool_context *tctx, const char *path);
void tool_context_set_path_approval(struct tool_context *tctx,
				    tool_path_approval_fn fn,
				    void *user_data);
void tool_context_add_read_allowed_dir(struct tool_context *tctx,
				       const char *dir);
void tool_context_add_allowed_dir(struct tool_context *tctx, const char *dir);

void tool_context_set_command_approval(struct tool_context *tctx,
				       tool_command_approval_fn fn,
				       void *user_data);
int tool_context_allow_command(struct tool_context *tctx, const char *pattern);
int tool_context_allow_exec_dir(struct tool_context *tctx, const char *path);
int tool_context_check_command(struct tool_context *tctx,
			       const char *command, const char *cwd);

#ifdef __cplusplus
}
#endif

#endif
