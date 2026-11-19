#ifndef MORPH_EXT_TYPES_H
#define MORPH_EXT_TYPES_H

/* Public ABI values: preserve existing values and layouts. */

enum permission_access {
	PERMISSION_READ_ONLY,
	PERMISSION_WORKSPACE,
	PERMISSION_FULL,
};

enum permission_approval {
	PERMISSION_ON_REQUEST,
	PERMISSION_NEVER,
};

enum permission_reviewer {
	PERMISSION_USER,
	PERMISSION_MODEL,
};

struct permission_policy {
	enum permission_access access;
	enum permission_approval approval;
	enum permission_reviewer reviewer;
};

enum tool_path_op {
	TOOL_PATH_READ = 0,
	TOOL_PATH_LIST = 1,
	TOOL_PATH_WRITE = 2,
	TOOL_PATH_DELETE = 3,
};

enum tool_artifact_kind {
	TOOL_ARTIFACT_FILE,
	TOOL_ARTIFACT_IMAGE,
	TOOL_ARTIFACT_VIDEO,
};

#endif
