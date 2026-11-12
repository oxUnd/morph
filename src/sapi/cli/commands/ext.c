#include "sapi/cli/commands/ext.h"
#include "sapi/cli/presentation/style.h"
#include "util/cleanup.h"
#include "ext/ext.h"
#include "ext/install.h"
#include "config/config.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/widgets/list.h"
#include "util/array.h"

struct cli_ext_tool_item {
	int index;
	int enabled;
	enum tool_origin origin;
	char name[TOOL_NAME_MAX];
};

struct cli_ext_tool_group {
	const char *name;
	int count;
	int rank;
};

static int cli_ext_origin_rank(enum tool_origin origin)
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

static const char *cli_ext_origin_display(enum tool_origin origin)
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

static int cli_ext_tool_compare(const void *left, const void *right)
{
	const struct cli_ext_tool_item *a = left;
	const struct cli_ext_tool_item *b = right;
	int rank_a = cli_ext_origin_rank(a->origin);
	int rank_b = cli_ext_origin_rank(b->origin);

	if (rank_a != rank_b)
		return rank_a - rank_b;
	if (a->enabled != b->enabled)
		return b->enabled - a->enabled;
	return strcmp(a->name, b->name);
}

static int cli_ext_collect_tools(struct cli_application *ctx, morph_array_t *items,
				 int *enabled_count)
{
	morph_array_t collected MORPH_CLEANUP(morph_array_cleanup) = {0};
	int count = runtime_tool_count(ctx->runtime);
	int rc;

	*enabled_count = 0;
	rc = morph_array_init(&collected, (size_t)(count > 0 ? count : 1),
			      sizeof(struct cli_ext_tool_item));
	if (rc != 0)
		return rc;
	for (int i = 0; i < count; i++) {
		struct cli_ext_tool_item *item;
		struct tool_desc desc;

		rc = runtime_tool_info_borrow(ctx->runtime, i, &desc);
		if (rc != 0)
			return rc;
		item = morph_array_push_borrow(&collected);
		if (!item) {
			rc = -ENOMEM;
			return rc;
		}
		memset(item, 0, sizeof(*item));
		item->index = i;
		strncpy(item->name, desc.name, sizeof(item->name) - 1);
		rc = runtime_tool_enabled(ctx->runtime, i, &item->enabled);
		if (rc != 0)
			return rc;
		rc = runtime_tool_origin(ctx->runtime, i, &item->origin);
		if (rc != 0)
			return rc;
		if (item->enabled)
			(*enabled_count)++;
	}
	qsort(collected.elts, collected.nelts, collected.size, cli_ext_tool_compare);
	return morph_array_take(items, &collected);
}

static const char *cli_ext_tool_marker(const struct cli_ext_tool_item *item)
{
	return item->enabled ? ANSI_GREEN CLI_FEED_DONE ANSI_RESET
			     : ANSI_DIM CLI_FEED_PENDING ANSI_RESET;
}

static int cli_ext_group_name_width(const morph_array_t *items, int rank)
{
	int width = 12;
	struct cli_ext_tool_item *item;

	morph_array_foreach (item, items, struct cli_ext_tool_item) {
		size_t item_width;

		if (cli_ext_origin_rank(item->origin) != rank)
			continue;
		item_width = utf8_display_width(item->name);
		if (item_width > (size_t)width)
			width = item_width > 28u ? 28 : (int)item_width;
	}
	return width;
}

static void cli_ext_print_group(struct cli_application *ctx, const morph_array_t *items,
				const struct cli_ext_tool_group *group, int is_last, int columns)
{
	struct cli_ext_tool_item *item;
	int index = 0;
	int name_width = cli_ext_group_name_width(items, group->rank);
	const char *ancestor = morph_tui_widget_tree_branch(is_last, 1);

	cli_list_group(ctx, group->name, group->count, is_last);
	morph_array_foreach (item, items, struct cli_ext_tool_item) {
		struct tool_desc desc;

		if (cli_ext_origin_rank(item->origin) != group->rank)
			continue;
		if (runtime_tool_info_borrow(ctx->runtime, item->index, &desc) != 0)
			continue;
		index++;
		cli_list_item(ctx, ancestor, index == group->count, cli_ext_tool_marker(item),
			      desc.name, desc.description, name_width, columns);
	}
}

static int cli_ext_print_tools(struct cli_application *ctx)
{
	static const char *const group_names[] = {"Built-in", "Extensions", "MCP", "Dynamic",
						  "Other"};
	struct cli_ext_tool_group groups[5];
	morph_array_t items MORPH_CLEANUP(morph_array_cleanup) = {0};
	int group_count = 0;
	int enabled_count;
	int columns = cli_list_columns();
	int rc;

	rc = cli_ext_collect_tools(ctx, &items, &enabled_count);
	if (rc != 0)
		return rc;
	CMD_HEADER(ctx, "Tools %zu" ANSI_DIM " · %d enabled" ANSI_RESET, items.nelts,
		   enabled_count);
	if (items.nelts == 0) {
		printf(ctx, "  " ANSI_DIM "(none)" ANSI_RESET "\n");
		return 0;
	}
	for (int rank = 0; rank < 5; rank++) {
		int count = 0;
		struct cli_ext_tool_item *item;

		morph_array_foreach (item, &items, struct cli_ext_tool_item) {
			if (cli_ext_origin_rank(item->origin) == rank)
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
			putchar(ctx, '\n');
		cli_ext_print_group(ctx, &items, &groups[i], i == group_count - 1, columns);
	}
	printf(ctx, "\n  " ANSI_DIM CLI_FEED_DONE " enabled   "
	       CLI_FEED_PENDING " disabled" ANSI_RESET "\n");
	return 0;
}

static int cli_ext_find_tool_index(struct cli_application *ctx, const char *name,
				   enum tool_origin *origin, unsigned *flags)
{
	int count = runtime_tool_count(ctx->runtime);

	for (int i = 0; i < count; i++) {
		struct tool_desc desc;

		if (runtime_tool_info_borrow(ctx->runtime, i, &desc) != 0 ||
		    strcmp(desc.name, name) != 0)
			continue;
		if (runtime_tool_origin(ctx->runtime, i, origin) != 0 ||
		    runtime_tool_flags(ctx->runtime, i, flags) != 0)
			return -EIO;
		return i;
	}
	return -ENOENT;
}

static int cli_ext_print_info(struct cli_application *ctx, const char *name)
{
	struct tool_desc desc;
	enum tool_origin origin;
	unsigned flags;
	int enabled;
	int columns = cli_list_columns();
	int schema_count;
	int index;

	if (runtime_tool_find_borrow(ctx->runtime, name, &desc) != 0) {
		CMD_ERROR(ctx, "tool not found: %s", name);
		return -ENOENT;
	}
	index = cli_ext_find_tool_index(ctx, name, &origin, &flags);
	if (index < 0)
		return index;
	if (runtime_tool_enabled(ctx->runtime, index, &enabled) != 0)
		return -EIO;
	CMD_HEADER(ctx, "Tool %s", desc.name);
	cli_list_value_field(ctx, "Status", enabled ? CLI_FEED_DONE " enabled"
						 : CLI_FEED_PENDING " disabled", 0, 12, columns);
	cli_list_value_field(ctx, "Source", cli_ext_origin_display(origin), 0, 12, columns);
	cli_list_value_field(ctx, "Access",
			     (flags & TOOL_FLAG_READONLY) ? "read-only" : "read-write", 0, 12,
			     columns);
	schema_count = (desc.input_schema[0] ? 1 : 0) + (desc.output_schema[0] ? 1 : 0);
	cli_list_text_field(ctx, "Description", desc.description, schema_count == 0, columns);
	if (desc.input_schema[0])
		cli_list_json_field(ctx, "Input schema", desc.input_schema, !desc.output_schema[0],
				    columns);
	if (desc.output_schema[0])
		cli_list_json_field(ctx, "Output schema", desc.output_schema, 1, columns);
	return 0;
}

static int cli_ext_command_ext(struct cli_application *ctx, int argc, char **argv)
{
	const char *sub = cli_cmd_arg_borrow(argc, argv, 1);
	if (sub && strcmp(sub, "list") == 0) {
		return cli_ext_print_tools(ctx);
	}
	if (sub && strcmp(sub, "info") == 0) {
		const char *name = cli_cmd_arg_borrow(argc, argv, 2);
		if (!name) {
			CMD_ERROR(ctx, "usage: /ext info <name>");
			return -EINVAL;
		}
		return cli_ext_print_info(ctx, name);
	}
	if (sub && strcmp(sub, "install") == 0) {
		const char *source = cli_cmd_arg_borrow(argc, argv, 2);
		int yes = 0;
		for (int i = 3; i < argc; i++) {
			if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0)
				yes = 1;
		}
		if (!source) {
			CMD_ERROR(ctx, "usage: /ext install <github-source-or-tree-url> [--yes]");
			return -EINVAL;
		}
		struct ext_install_options opts;
		memset(&opts, 0, sizeof(opts));
		const struct config *config = runtime_config_borrow(ctx->runtime);
		opts.install_dir = config->ext.dir[0] ? config->ext.dir : "~/.morph/exts";
		opts.yes = yes;
		opts.in = stdin;
		opts.out = stdout;
		struct ext_install_result res;
		int rc = ext_install_source(ctx ? ctx->logger : NULL, source, &opts, &res);
		if (rc < 0) {
			CMD_ERROR(ctx, "ext install failed: %s", morph_strerror(rc));
			return rc;
		}
		CMD_OK(ctx, "installed ext %s to %s", res.name, res.path);
		if (res.resolved_ref[0])
			printf(ctx, "  resolved_ref: %s\n", res.resolved_ref);
		return 0;
	}
	if (sub && strcmp(sub, "remove") == 0) {
		CMD_ERROR(ctx, "ext remove not yet implemented (M4)");
		return 0;
	}
	if (sub && strcmp(sub, "enable") == 0) {
		CMD_ERROR(ctx, "ext enable not yet implemented (M4)");
		return 0;
	}
	if (sub && strcmp(sub, "disable") == 0) {
		CMD_ERROR(ctx, "ext disable not yet implemented (M4)");
		return 0;
	}
	return cli_ext_print_tools(ctx);
}

static const struct cli_command cli_ext_entries[] = {
	{"/ext", cli_ext_command_ext, "List or manage tools and exts", "/ext list"},
	{"/x", cli_ext_command_ext, "Alias for /ext", "/x list"},
};

struct cli_command_group cli_ext_commands(void)
{
	return (struct cli_command_group){
		.entries = cli_ext_entries,
		.count = (int)(sizeof(cli_ext_entries) / sizeof(cli_ext_entries[0])),
		.category = "Extensions",
	};
}
