#include "sapi/cli/component/mcp_tree.h"
#include <nb/err.h>
#include "sapi/cli/presentation/mcp_tree.h"
#include "sapi/cli/component/report.h"
#include "sapi/cli/presentation/style.h"
#include "runtime/runtime.h"
#include <nb/cleanup.h>
#include <stdlib.h>
#include <string.h>

struct cli_mcp_tree {
	struct cli_report report;
	nb_vec servers, items;
	char *kind, *server;
};

static int cli_mcp_tree_format(struct cli_report *report, nb_buf *out, int width)
{
	struct cli_mcp_tree *tree = (struct cli_mcp_tree *)report;
	if (tree->kind)
		return cli_mcp_tree_items_render(out, tree->kind, tree->server,
						 (void *)tree->items.data, tree->items.len,
						 width > 0 ? width : 1, report->styled);
	return cli_mcp_tree_render(out, (void *)tree->servers.data, tree->servers.len,
				   width > 0 ? width : 1, report->styled);
}

static void cli_mcp_tree_destroy(struct nt_view *view)
{
	struct cli_mcp_tree *tree = (struct cli_mcp_tree *)view;
	struct cli_mcp_tree_item *item;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&tree->items) &&
			     (item = (struct cli_mcp_tree_item *)nb_vec_at(&tree->items, _i_));
	     _i_++) {
		free((char *)item->name);
		free((char *)item->description);
		free((char *)item->uri);
	}
	nb_vec_cleanup(&tree->items);
	nb_vec_cleanup(&tree->servers);
	free(tree->kind);
	free(tree->server);
}

static struct cli_mcp_tree *cli_mcp_tree_add(struct nt_builder *builder, struct nt_view *parent,
					     int styled)
{
	static const struct nt_view_class cli_mcp_tree_class = {.superclass = &cli_report_class,
								.instance_size =
									sizeof(struct cli_mcp_tree),
								.kind = "cli_mcp_tree",
								.deinit = cli_mcp_tree_destroy};
	const struct nt_view_options options = {
		.padding = {.right = (int)CLI_CONTENT_RIGHT_PADDING}};
	struct cli_mcp_tree *tree = (struct cli_mcp_tree *)cli_report_add(
		builder, parent, &cli_mcp_tree_class, &options, styled, cli_mcp_tree_format);
	if (!tree)
		return NULL;
	return tree;
}

struct nt_view *cli_mcp_tree_servers_add_borrow(struct nt_builder *builder, struct nt_view *parent,
						const struct runtime_mcp_status *servers,
						size_t count, int styled)
{
	if (!servers && count) {
		nt_builder_fail(builder, NB_ERR_INVAL);
		return NULL;
	}
	struct cli_mcp_tree *tree = cli_mcp_tree_add(builder, parent, styled);
	if (!tree)
		return NULL;
	struct runtime_mcp_status snapshot;
	int rc = (nb_vec_init(&tree->servers, sizeof(snapshot)),
		  nb_vec_reserve(&tree->servers, count ? count : 1) ? 0 : NB_ERR_NOMEM);
	for (size_t i = 0; !rc && i < count; i++) {
		struct runtime_mcp_status *copy = nb_vec_push(&tree->servers);
		if (!copy) {
			rc = NB_ERR_NOMEM;
			break;
		}
		/* Copy display fields only; credentials and process configuration
		 * do not belong to the retained report. */
		copy->state = servers[i].state;
		copy->connected = servers[i].connected;
		copy->error_code = servers[i].error_code;
		copy->elapsed_ms = servers[i].elapsed_ms;
		copy->config.transport = servers[i].config.transport;
		memcpy(copy->config.name, servers[i].config.name, sizeof(copy->config.name));
		memcpy(copy->server_name, servers[i].server_name, sizeof(copy->server_name));
		memcpy(copy->server_version, servers[i].server_version,
		       sizeof(copy->server_version));
		memcpy(copy->negotiated_version, servers[i].negotiated_version,
		       sizeof(copy->negotiated_version));
		copy->supports_tools = servers[i].supports_tools;
		copy->supports_resources = servers[i].supports_resources;
		copy->supports_prompts = servers[i].supports_prompts;
	}
	nt_builder_fail(builder, rc);
	return &tree->report.base;
}

struct nt_view *cli_mcp_tree_items_add_borrow(struct nt_builder *builder, struct nt_view *parent,
					      const char *kind, const char *server,
					      const struct cli_mcp_tree_item *items, size_t count,
					      int styled)
{
	if (!kind || !server || (!items && count)) {
		nt_builder_fail(builder, NB_ERR_INVAL);
		return NULL;
	}
	struct cli_mcp_tree *tree = cli_mcp_tree_add(builder, parent, styled);
	if (!tree)
		return NULL;
	tree->kind = strdup(kind);
	tree->server = strdup(server);
	struct cli_mcp_tree_item item;
	int rc = tree->kind && tree->server
			 ? (nb_vec_init(&tree->items, sizeof(item)),
			    nb_vec_reserve(&tree->items, count ? count : 1) ? 0 : NB_ERR_NOMEM)
			 : NB_ERR_NOMEM;
	for (size_t i = 0; !rc && i < count; i++) {
		struct cli_mcp_tree_item *copy = nb_vec_push(&tree->items);
		if (!copy) {
			rc = NB_ERR_NOMEM;
			break;
		}
		copy->name = strdup(items[i].name ? items[i].name : "");
		copy->description = strdup(items[i].description ? items[i].description : "");
		copy->uri = strdup(items[i].uri ? items[i].uri : "");
		if (!copy->name || !copy->description || !copy->uri)
			rc = NB_ERR_NOMEM;
	}
	nt_builder_fail(builder, rc);
	return &tree->report.base;
}
