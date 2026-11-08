#ifndef MORPH_DYNAMIC_TOOL_INTERNAL_H
#define MORPH_DYNAMIC_TOOL_INTERNAL_H

#include "dynamic_tools.h"
#include "util/buf.h"
#include <limits.h>
#include <stdint.h>

#define DYN_TOOL_META_FILE "tool.json"
#define DYN_TOOL_SOURCE_FILE "tool.js"
#define DYN_TOOL_HISTORY_DIR ".history"
#define DYN_TOOL_CHECKPOINT_META_FILE "checkpoint.json"
#define DYN_TOOL_OUTPUT_LIMIT (1024 * 1024)
#define DYN_TOOL_HISTORY_LIMIT 20

struct dynamic_tool {
	char *source;
	char name[TOOL_NAME_MAX];
	char *description;
	char *input_schema;
	char *output_schema;
	char source_path[PATH_MAX];
	char dir[PATH_MAX];
	const struct config_dynamic_tools *cfg;
	struct tool_context *tctx;
	enum tool_origin origin;
};

struct dynamic_tools_context {
	unsigned int refs;
	struct tool_registry *reg;
	struct tool_context *tctx;
	const struct config_dynamic_tools *cfg;
	char session_id[128];
};

struct tool_checkpoint {
	char id[16];
	char name[TOOL_NAME_MAX];
	char before_state[16];
	char origin[32];
	char old_hash[32];
	char new_hash[32];
	long created_at;
};

#endif
