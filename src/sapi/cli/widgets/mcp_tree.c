#include "sapi/cli/widgets/mcp_tree.h"
#include "sapi/cli/presentation/mcp_tree.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/style.h"
#include "runtime/runtime.h"
#include "util/cleanup.h"
#include <morph/tui/widgets/text_view.h>
#include <stdlib.h>
#include <string.h>

struct cli_mcp_tree {
	struct morph_tui_widget base;
	struct morph_tui_view *body;
	morph_array_t servers, items;
	char *kind, *server;
	int columns, styled;
};

static int cli_mcp_tree_measure(struct morph_tui_view *view, int width,
	struct morph_tui_view_size *size)
{
	struct cli_mcp_tree *tree = (struct cli_mcp_tree *)view;
	if (tree->columns != width) {
		morph_buf_t rendered MORPH_CLEANUP(morph_buf_cleanup) = {0};
		int rc = tree->kind
			? cli_mcp_tree_items_render(&rendered, tree->kind, tree->server,
				tree->items.elts, tree->items.nelts, width > 0 ? width : 1,
				tree->styled)
			: cli_mcp_tree_render(&rendered, tree->servers.elts, tree->servers.nelts,
				width > 0 ? width : 1, tree->styled);
		if (!rc)
			rc = cli_messages_set_rendered_view(tree->body, rendered.data,
				tree->styled);
		if (rc)
			return rc;
		tree->columns = width;
	}
	return morph_tui_view_measure(tree->body, width, size);
}

static int cli_mcp_tree_layout(struct morph_tui_view *view)
{
	return morph_tui_view_arrange(((struct cli_mcp_tree *)view)->body,
		morph_tui_view_content_rect(view));
}

static void cli_mcp_tree_destroy(struct morph_tui_view *view)
{
	struct cli_mcp_tree *tree = (struct cli_mcp_tree *)view;
	struct cli_mcp_tree_item *item;
	morph_array_foreach (item, &tree->items, struct cli_mcp_tree_item) {
		free((char *)item->name);
		free((char *)item->description);
		free((char *)item->uri);
	}
	morph_array_cleanup(&tree->items);
	morph_array_cleanup(&tree->servers);
	free(tree->kind);
	free(tree->server);
}

static struct cli_mcp_tree *cli_mcp_tree_add(struct morph_tui_builder *builder,
	struct morph_tui_view *parent, int styled)
{
	static const struct morph_tui_view_ops cli_mcp_tree_ops = {
		.measure = cli_mcp_tree_measure, .layout = cli_mcp_tree_layout,
		.destroy = cli_mcp_tree_destroy};
	const struct morph_tui_view_options options = {
		.padding = {.right = (int)CLI_CONTENT_RIGHT_PADDING}};
	struct cli_mcp_tree *tree = (struct cli_mcp_tree *)morph_tui_widget_add_borrow(
		builder, parent, sizeof(*tree), &cli_mcp_tree_ops, &options);
	if (!tree)
		return NULL;
	tree->columns = -1;
	tree->styled = styled;
	tree->body = morph_tui_widget_text_view_add_borrow(builder, &tree->base.view, "", NULL);
	if (tree->body) {
		morph_tui_view_set_focusable(tree->body, 0);
		morph_tui_builder_fail(builder,
			morph_tui_widget_text_view_set_content_sizing(tree->body, 1));
	}
	return tree;
}

struct morph_tui_view *cli_mcp_tree_servers_add_borrow(struct morph_tui_builder *builder,
	struct morph_tui_view *parent, const struct runtime_mcp_status *servers,
	size_t count, int styled)
{
	if (!servers && count) {
		morph_tui_builder_fail(builder, -EINVAL);
		return NULL;
	}
	struct cli_mcp_tree *tree = cli_mcp_tree_add(builder, parent, styled);
	if (!tree)
		return NULL;
	struct runtime_mcp_status snapshot;
	int rc = morph_array_init(&tree->servers, count ? count : 1, sizeof(snapshot));
	for (size_t i = 0; !rc && i < count; i++) {
		struct runtime_mcp_status *copy = morph_array_push_borrow(&tree->servers);
		if (!copy) {
			rc = -ENOMEM;
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
	morph_tui_builder_fail(builder, rc);
	return &tree->base.view;
}

struct morph_tui_view *cli_mcp_tree_items_add_borrow(struct morph_tui_builder *builder,
	struct morph_tui_view *parent, const char *kind, const char *server,
	const struct cli_mcp_tree_item *items, size_t count, int styled)
{
	if (!kind || !server || (!items && count)) {
		morph_tui_builder_fail(builder, -EINVAL);
		return NULL;
	}
	struct cli_mcp_tree *tree = cli_mcp_tree_add(builder, parent, styled);
	if (!tree)
		return NULL;
	tree->kind = strdup(kind);
	tree->server = strdup(server);
	struct cli_mcp_tree_item item;
	int rc = tree->kind && tree->server
		? morph_array_init(&tree->items, count ? count : 1, sizeof(item)) : -ENOMEM;
	for (size_t i = 0; !rc && i < count; i++) {
		struct cli_mcp_tree_item *copy = morph_array_push_borrow(&tree->items);
		if (!copy) {
			rc = -ENOMEM;
			break;
		}
		copy->name = strdup(items[i].name ? items[i].name : "");
		copy->description = strdup(items[i].description ? items[i].description : "");
		copy->uri = strdup(items[i].uri ? items[i].uri : "");
		if (!copy->name || !copy->description || !copy->uri)
			rc = -ENOMEM;
	}
	morph_tui_builder_fail(builder, rc);
	return &tree->base.view;
}
