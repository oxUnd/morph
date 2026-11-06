#ifndef MORPH_PERMISSION_OPERATION_H
#define MORPH_PERMISSION_OPERATION_H

#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

enum tool_path_op {
	TOOL_PATH_READ = 0,
	TOOL_PATH_LIST = 1,
	TOOL_PATH_WRITE = 2,
	TOOL_PATH_DELETE = 3,
};

enum tool_path_access {
	TOOL_PATH_ACCESS_READ = 1u << 0,
	TOOL_PATH_ACCESS_WRITE = 1u << 1,
	TOOL_PATH_ACCESS_DELETE = 1u << 2,
	TOOL_PATH_ACCESS_ALL = TOOL_PATH_ACCESS_READ | TOOL_PATH_ACCESS_WRITE |
		TOOL_PATH_ACCESS_DELETE,
};

struct morph_log;
struct permission_review;

struct tool_directory_access_request {
	unsigned int access;
	const char *principal;
	const char *command;
	const char *path;
	const char *reason;
	int session_scope;
};

enum tool_operation_kind {
	TOOL_OP_COMMAND = 0,
	TOOL_OP_PATH_READ = 1,
	TOOL_OP_PATH_LIST = 2,
	TOOL_OP_PATH_WRITE = 3,
	TOOL_OP_NETWORK = 4,
	TOOL_OP_EXTERNAL_SEND = 5,
	TOOL_OP_PATH_DELETE = 6,
	TOOL_OP_DIRECTORY_ACCESS = 7,
	TOOL_OP_TOOL_CALL = 8,
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
	const char *reason;
	const char *const *programs;
	int programs_count;
	/* Explicit directory permissions shown together in one approval. */
	unsigned int path_access;
	int host;
	int network;
	int require_user;
	int require_review;
	int forbidden;
	/* Optional caller-owned decision, copied before releasing the review lock. */
	struct permission_review *review_out;
};

typedef enum tool_operation_verdict (*tool_operation_approval_fn)(
	const struct tool_operation *op, void *user_data);

#ifdef __cplusplus
}
#endif
#endif
