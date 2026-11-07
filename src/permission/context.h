#ifndef MORPH_PERMISSION_CONTEXT_H
#define MORPH_PERMISSION_CONTEXT_H

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

#include "permission/permission.h"

struct db;
struct arena;

struct tool_context;

struct tool_path_snapshot {
	char **paths;
	int count;
};

struct tool_permission_snapshot {
	struct permission_policy policy;
	struct morph_log *logger;
	const char *workdir;
	const char *output_dir;
	struct tool_path_snapshot read;
	struct tool_path_snapshot write;
	struct tool_path_snapshot remove;
};

/* Strings and arrays belong to arena and survive subsequent grant changes. */
int tool_context_snapshot_permissions(const struct tool_context *tctx, const char *principal,
				      struct arena *arena,
				      struct tool_permission_snapshot *snapshot);

int tool_context_review(struct tool_context *tctx, const struct tool_operation *op,
	enum tool_operation_verdict *verdict);
int tool_context_set_permission_policy(struct tool_context *tctx,
	const struct permission_policy *policy);
int tool_context_get_permission_policy(const struct tool_context *tctx,
				       struct permission_policy *policy);
void tool_context_get_operation_approval(struct tool_context *tctx,
					 tool_operation_approval_fn *callback, void **user_data);
void tool_context_set_review_adapters(struct tool_context *tctx, permission_review_fn user_review,
				      void *user_data, permission_review_fn model_review,
				      void *model_data);
void tool_context_reset_denials(struct tool_context *tctx);
/* Borrowed by the agent; configuration belongs to the context owner. */
struct permission_context *tool_context_permission_context(struct tool_context *tctx);
struct tool_context *tool_context_create(struct morph_log *logger, const char *workdir,
					 const char *output_dir);
void tool_context_destroy(struct tool_context *tctx);
const char *tool_context_workdir(const struct tool_context *tctx);
const char *tool_context_output_dir(const struct tool_context *tctx);
const char *tool_context_grant_project_root(const struct tool_context *tctx);
struct morph_log *tool_context_logger(const struct tool_context *tctx);
void tool_context_set_default_timeout(struct tool_context *tctx, int seconds);
int tool_context_default_timeout(const struct tool_context *tctx);
/* Built-in policy paths are independent of bounded user approval grants. */
int tool_context_add_builtin_path(struct tool_context *tctx,
				 enum tool_path_op op, const char *path);
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
/* Append all matching paths to a caller-initialized array of const char *. */
int tool_context_collect_path_grants(const struct tool_context *tctx,
	const char *principal, enum tool_path_op op, morph_array_t *paths);
int tool_context_collect_write_grants(const struct tool_context *tctx,
				      const char *principal,
				      const char **paths, int max_paths);
int tool_context_add_exec_profile_path(struct tool_context *tctx,
				       enum tool_path_op op,
				       const char *path);
int tool_context_request_delete_access(struct tool_context *tctx,
				       const char *principal,
				       const char *command,
				       const char *path,
				       char *resolved,
				       size_t resolved_size);
int tool_context_grant_delete_access(struct tool_context *tctx,
				     const char *principal, const char *path,
				     char *resolved, size_t resolved_size);
/* Directory batches preserve independent read/write/delete grants. */
const char *tool_path_access_name(unsigned int access);
int tool_context_resolve_directory_access(const char *path, unsigned int access,
	char *resolved, size_t resolved_size);
int tool_context_request_directory_access(struct tool_context *tctx,
	const struct tool_directory_access_request *request,
	char *resolved, size_t resolved_size);
/* Use principal "*" for directory grants shared by tools and commands. */
int tool_context_request_scoped_access(struct tool_context *tctx,
				       enum tool_path_op operation,
				       const char *principal,
				       const char *command, const char *path,
				       int session_scope,
				       char *resolved, size_t resolved_size);
void tool_context_clear_turn_grants(struct tool_context *tctx);
int tool_context_collect_delete_grants(const struct tool_context *tctx,
				       const char *principal,
				       const char **paths, int max_paths);

#ifdef __cplusplus
}
#endif

#endif
