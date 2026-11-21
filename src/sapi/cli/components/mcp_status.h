#ifndef MORPH_CLI_COMPONENTS_MCP_STATUS_H
#define MORPH_CLI_COMPONENTS_MCP_STATUS_H

#include <nothing/tui/view.h>

struct cli_notice;

struct nt_view *cli_mcp_status_add(struct nt_builder *builder, struct nt_view *parent,
	int styled);
/* Copies all notice strings. Existing servers retain their row position. */
int cli_mcp_status_update(struct nt_view *view, const char *server,
	const struct cli_notice *notice);

#endif
