#ifndef MORPH_CLI_PRESENTATION_MCP_TREE_H
#define MORPH_CLI_PRESENTATION_MCP_TREE_H

#include "util/buf.h"
#include <stddef.h>

struct runtime_mcp_status;
struct cli_application;

struct cli_mcp_tree_item {
	const char *name, *description, *uri;
};

/* columns is content width; the enclosing component owns its insets. */
int cli_mcp_tree_render(morph_buf_t *, const struct runtime_mcp_status *, size_t count,
	int columns, int styled);
int cli_mcp_tree_items_render(morph_buf_t *, const char *kind, const char *server,
	const struct cli_mcp_tree_item *, size_t count, int columns, int styled);
int cli_mcp_tree_write(struct cli_application *, const struct runtime_mcp_status *, size_t count);
int cli_mcp_tree_items_write(struct cli_application *, const char *kind, const char *server,
	const struct cli_mcp_tree_item *, size_t count);

#endif
