#ifndef TOOL_CONTEXT_H
#define TOOL_CONTEXT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <stddef.h>
#include "util/array.h"

#define TOOL_CONTEXT_OUTPUT_DIR_MAX PATH_MAX
#define TOOL_CONTEXT_ALLOW_MAX 32
#define TOOL_CONTEXT_ALLOW_PATH_MAX PATH_MAX
#define TOOL_CONTEXT_ACTION_MAX 1024
#define TOOL_CONTEXT_ENV_NAME_MAX 128
#define TOOL_CONTEXT_CLI_NAME_MAX 256
#define TOOL_CONTEXT_CLI_DIR_MAX 6

enum tool_path_op {
	TOOL_PATH_READ = 0,
	TOOL_PATH_LIST = 1,
	TOOL_PATH_WRITE = 2,
	TOOL_PATH_DELETE = 3,
};

enum tool_operation_kind {
	TOOL_OP_COMMAND = 0,
	TOOL_OP_PATH_READ = 1,
	TOOL_OP_PATH_LIST = 2,
	TOOL_OP_PATH_WRITE = 3,
	TOOL_OP_NETWORK = 4,
	TOOL_OP_EXTERNAL_SEND = 5,
	TOOL_OP_PATH_DELETE = 6,
};

enum tool_operation_verdict {
	TOOL_OP_DENY = 0,
	TOOL_OP_ALLOW = 1,
	TOOL_OP_SESSION = 2,
	TOOL_OP_ALWAYS = 3,
};

struct tool_operation {
	enum tool_operation_kind kind;
	const char *tool_name;
	const char *principal;
	const char *action;
	const char *target;
	const char *scope;
	const char *details_json;
	const struct tool_directory_capability *directories;
	int directories_count;
};

typedef enum tool_operation_verdict (*tool_operation_approval_fn)(
	const struct tool_operation *op, void *user_data);

struct db;

struct tool_directory_capability {
	char path[PATH_MAX];
	int create;
};

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
	morph_array_t scoped_grants;
	morph_array_t persistent_grants;
	struct db *grant_db;
	char grant_project_root[TOOL_CONTEXT_ALLOW_PATH_MAX];
	int default_timeout_seconds;
	int bash_exec_local_mode;
	int bash_exec_mode_configured;
	int bash_exec_server_network_access;
	char bash_exec_server_read_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int bash_exec_server_read_dirs_count;
	char bash_exec_server_write_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int bash_exec_server_write_dirs_count;
	char bash_exec_server_delete_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int bash_exec_server_delete_dirs_count;
	char bash_exec_server_allowed_env[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ENV_NAME_MAX];
	int bash_exec_server_allowed_env_count;
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
int tool_context_check_operation_verdict(
	struct tool_context *tctx, const struct tool_operation *op,
	enum tool_operation_verdict *verdict);
void tool_context_set_operation_approval(struct tool_context *tctx,
					 tool_operation_approval_fn fn,
					 void *user_data);
int tool_context_set_grant_store(struct tool_context *tctx, struct db *db,
				 const char *project_root);
int tool_context_reload_persistent_grants(struct tool_context *tctx);
void tool_context_add_read_allowed_dir(struct tool_context *tctx,
				       const char *dir);
void tool_context_add_write_allowed_dir(struct tool_context *tctx,
					const char *dir);
int tool_context_allow_command_pattern(struct tool_context *tctx,
				       const char *pattern);
int tool_context_allow_command_scope(struct tool_context *tctx,
				     const char *path);
int tool_context_command_name(const char *command, char *out,
			      size_t out_size);
int tool_context_command_principal(const char *command, char *out,
				   size_t out_size);
int tool_context_discover_cli_dirs(
	const struct tool_context *tctx, const char *command,
	struct tool_directory_capability *directories, int max_directories);
int tool_context_grant_write_access(
	struct tool_context *tctx, const char *principal, const char *path,
	int create, enum tool_operation_verdict verdict,
	char *resolved, size_t resolved_size);
int tool_context_request_write_access(struct tool_context *tctx,
				      const char *principal,
				      const char *command,
				      const char *path,
				      char *resolved,
				      size_t resolved_size);
int tool_context_collect_write_grants(const struct tool_context *tctx,
				      const char *principal,
				      const char **paths, int max_paths);
void tool_context_set_bash_exec_mode(struct tool_context *tctx,
				     const char *mode);
void tool_context_set_bash_exec_server_network(struct tool_context *tctx,
					       int enabled);
int tool_context_add_bash_exec_server_env(struct tool_context *tctx,
					  const char *name);
int tool_context_add_bash_exec_server_path(struct tool_context *tctx,
					   enum tool_path_op op,
					   const char *path);
int tool_context_request_delete_access(struct tool_context *tctx,
				       const char *principal,
				       const char *command,
				       const char *path,
				       char *resolved,
				       size_t resolved_size);
int tool_context_collect_delete_grants(const struct tool_context *tctx,
				       const char *principal,
				       const char **paths, int max_paths);

#ifdef __cplusplus
}
#endif

#endif
