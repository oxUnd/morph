#ifndef MORPH_CLI_COMPONENT_MCP_TREE_H
#define MORPH_CLI_COMPONENT_MCP_TREE_H

#include <nothing/tui/view.h>
#include "sapi/cli/presentation/mcp_tree.h"

/* Controller-owned components copy the report snapshot and reflow on resize. */
struct nt_view *cli_mcp_tree_servers_add_borrow(struct nt_builder *, struct nt_view *parent,
						const struct runtime_mcp_status *, size_t count,
						int styled);
struct nt_view *cli_mcp_tree_items_add_borrow(struct nt_builder *, struct nt_view *parent,
					      const char *kind, const char *server,
					      const struct cli_mcp_tree_item *, size_t count,
					      int styled);

#endif
