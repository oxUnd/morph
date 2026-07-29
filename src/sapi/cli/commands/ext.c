#include "sapi/cli/commands/registry.h"
#include "sapi/cli/list_ui.h"
#include "util/array.h"

struct ext_tool_item {
	int index;
	enum tool_origin origin;
	char name[TOOL_NAME_MAX];
};

struct ext_tool_group {
	const char *name;
	int count;
	int rank;
};

static int ext_origin_rank(enum tool_origin origin)
{
	switch (origin) {
	case TOOL_ORIGIN_BUILTIN:
		return 0;
	case TOOL_ORIGIN_EXT:
		return 1;
	case TOOL_ORIGIN_MCP:
		return 2;
	case TOOL_ORIGIN_DYNAMIC_SESSION:
	case TOOL_ORIGIN_DYNAMIC_PERSISTENT:
		return 3;
	default:
		return 4;
	}
}

static const char *ext_origin_display(enum tool_origin origin)
{
	switch (origin) {
	case TOOL_ORIGIN_BUILTIN:
		return "Built-in";
	case TOOL_ORIGIN_EXT:
		return "Extension";
	case TOOL_ORIGIN_MCP:
		return "MCP";
	case TOOL_ORIGIN_DYNAMIC_SESSION:
		return "Dynamic session";
	case TOOL_ORIGIN_DYNAMIC_PERSISTENT:
		return "Dynamic persistent";
	default:
		return "Other";
	}
}

static int ext_tool_compare(const void *left, const void *right)
{
	const struct ext_tool_item *a = left;
	const struct ext_tool_item *b = right;
	int rank_a = ext_origin_rank(a->origin);
	int rank_b = ext_origin_rank(b->origin);

	if (rank_a != rank_b)
		return rank_a - rank_b;
	return strcmp(a->name, b->name);
}

static int ext_collect_tools(struct cli_context *ctx, morph_array_t *items)
{
	int count = runtime_tool_count(ctx->runtime);
	int rc;

	rc = morph_array_init(items, (size_t)(count > 0 ? count : 1),
			      sizeof(struct ext_tool_item));
	if (rc != 0)
		return rc;
	for (int i = 0; i < count; i++) {
		struct ext_tool_item *item;
		struct tool_desc desc;

		rc = runtime_tool_info(ctx->runtime, i, &desc);
		if (rc != 0)
			goto out;
		item = morph_array_push(items);
		if (!item) {
			rc = -ENOMEM;
			goto out;
		}
		memset(item, 0, sizeof(*item));
		item->index = i;
		strncpy(item->name, desc.name, sizeof(item->name) - 1);
		rc = runtime_tool_origin(ctx->runtime, i, &item->origin);
		if (rc != 0)
			goto out;
	}
	qsort(items->elts, items->nelts, items->size, ext_tool_compare);
	return 0;
out:
	morph_array_cleanup(items);
	return rc;
}

static int ext_group_name_width(const morph_array_t *items, int rank)
{
	int width = 12;
	struct ext_tool_item *item;

	morph_array_foreach(item, items, struct ext_tool_item) {
		size_t item_width;

		if (ext_origin_rank(item->origin) != rank)
			continue;
		item_width = utf8_display_width(item->name);
		if (item_width > (size_t)width)
			width = item_width > 28u ? 28 : (int)item_width;
	}
	return width;
}

static void ext_print_group(struct cli_context *ctx,
			    const morph_array_t *items,
			    const struct ext_tool_group *group,
			    int is_last, int columns)
{
	struct ext_tool_item *item;
	int index = 0;
	int name_width = ext_group_name_width(items, group->rank);
	const char *ancestor = is_last ? "  " : "│ ";

	cli_list_group(group->name, group->count, is_last);
	morph_array_foreach(item, items, struct ext_tool_item) {
		struct tool_desc desc;

		if (ext_origin_rank(item->origin) != group->rank)
			continue;
		if (runtime_tool_info(ctx->runtime, item->index, &desc) != 0)
			continue;
		index++;
		cli_list_item(ancestor, index == group->count, NULL,
			      desc.name, desc.description, name_width,
			      columns);
	}
}

static int ext_print_tools(struct cli_context *ctx)
{
	static const char *group_names[] = {
		"Built-in", "Extensions", "MCP", "Dynamic", "Other"
	};
	struct ext_tool_group groups[5];
	morph_array_t items;
	int group_count = 0;
	int columns = cli_list_columns();
	int rc;

	memset(&items, 0, sizeof(items));
	rc = ext_collect_tools(ctx, &items);
	if (rc != 0)
		return rc;
	CMD_HEADER("Tools %zu", items.nelts);
	if (items.nelts == 0) {
		printf("  " ANSI_DIM "(none)" ANSI_RESET "\n");
		goto out;
	}
	for (int rank = 0; rank < 5; rank++) {
		int count = 0;
		struct ext_tool_item *item;

		morph_array_foreach(item, &items, struct ext_tool_item) {
			if (ext_origin_rank(item->origin) == rank)
				count++;
		}
		if (count == 0)
			continue;
		groups[group_count].name = group_names[rank];
		groups[group_count].count = count;
		groups[group_count].rank = rank;
		group_count++;
	}
	for (int i = 0; i < group_count; i++) {
		if (i > 0)
			putchar('\n');
		ext_print_group(ctx, &items, &groups[i],
				i == group_count - 1, columns);
	}
out:
	morph_array_cleanup(&items);
	return 0;
}

static int ext_find_tool_index(struct cli_context *ctx, const char *name,
			       enum tool_origin *origin, unsigned *flags)
{
	int count = runtime_tool_count(ctx->runtime);

	for (int i = 0; i < count; i++) {
		struct tool_desc desc;

		if (runtime_tool_info(ctx->runtime, i, &desc) != 0 ||
		    strcmp(desc.name, name) != 0)
			continue;
		if (runtime_tool_origin(ctx->runtime, i, origin) != 0 ||
		    runtime_tool_flags(ctx->runtime, i, flags) != 0)
			return -EIO;
		return i;
	}
	return -ENOENT;
}

static int ext_print_info(struct cli_context *ctx, const char *name)
{
	struct tool_desc desc;
	enum tool_origin origin;
	unsigned flags;
	int columns = cli_list_columns();
	int schema_count;
	int index;

	if (runtime_tool_find(ctx->runtime, name, &desc) != 0) {
		CMD_ERROR("tool not found: %s", name);
		return -ENOENT;
	}
	index = ext_find_tool_index(ctx, name, &origin, &flags);
	if (index < 0)
		return index;
	CMD_HEADER("Tool %s", desc.name);
	cli_list_value_field("Source", ext_origin_display(origin), 0, 12,
			     columns);
	cli_list_value_field("Access",
			     (flags & TOOL_FLAG_READONLY) ?
			     "read-only" : "read-write",
			     0, 12, columns);
	schema_count = (desc.input_schema[0] ? 1 : 0) +
		(desc.output_schema[0] ? 1 : 0);
	cli_list_text_field("Description", desc.description,
			    schema_count == 0, columns);
	if (desc.input_schema[0])
		cli_list_json_field("Input schema", desc.input_schema,
				    !desc.output_schema[0], columns);
	if (desc.output_schema[0])
		cli_list_json_field("Output schema", desc.output_schema, 1,
				    columns);
	return 0;
}

static int cmd_ext(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = cli_cmd_arg(argc, argv, 1);
	if (sub && strcmp(sub, "list") == 0) {
		return ext_print_tools(ctx);
	}
	if (sub && strcmp(sub, "info") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /ext info <name>");
			return -EINVAL;
		}
		return ext_print_info(ctx, name);
	}
	if (sub && strcmp(sub, "install") == 0) {
		const char *source = cli_cmd_arg(argc, argv, 2);
		int yes = 0;
		for (int i = 3; i < argc; i++) {
			if (strcmp(argv[i], "--yes") == 0 ||
			    strcmp(argv[i], "-y") == 0)
				yes = 1;
		}
		if (!source) {
			CMD_ERROR("usage: /ext install <github-source-or-tree-url> [--yes]");
			return -EINVAL;
		}
		struct ext_install_options opts;
		memset(&opts, 0, sizeof(opts));
		const struct config *config = runtime_config_get(ctx->runtime);
		opts.install_dir = config->ext.dir[0] ? config->ext.dir : "~/.morph/exts";
		opts.yes = yes;
		opts.in = stdin;
		opts.out = stdout;
		struct ext_install_result res;
		int rc = ext_install_source(source, &opts, &res);
		if (rc < 0) {
			CMD_ERROR("ext install failed: %s", morph_strerror(rc));
			return rc;
		}
		CMD_OK("installed ext %s to %s", res.name, res.path);
		if (res.resolved_ref[0])
			printf("  resolved_ref: %s\n", res.resolved_ref);
		return 0;
	}
	if (sub && strcmp(sub, "remove") == 0) {
		CMD_ERROR("ext remove not yet implemented (M4)");
		return 0;
	}
	if (sub && strcmp(sub, "enable") == 0) {
		CMD_ERROR("ext enable not yet implemented (M4)");
		return 0;
	}
	if (sub && strcmp(sub, "disable") == 0) {
		CMD_ERROR("ext disable not yet implemented (M4)");
		return 0;
	}
	return ext_print_tools(ctx);
}


static const struct cli_command ext_commands[] = {
	{ "/ext",     cmd_ext,     "List or manage tools and exts",     "/ext list" },
	{ "/x",       cmd_ext,     "Alias for /ext",                    "/x list" },
};

int cli_register_ext_commands(void)
{
	return cli_command_register_many(ext_commands,
					 (int)(sizeof(ext_commands) /
					 sizeof(ext_commands[0])),
					 "Extensions");
}
