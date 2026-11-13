#include "sapi/cli/commands/ext.h"
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/command_output.h"
#include "sapi/cli/presentation/style.h"
#include "ext/ext.h"
#include "ext/install.h"
#include "config/config.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/widgets/list.h"

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
	const struct cli_tool_list_row *a = left;
	const struct cli_tool_list_row *b = right;

	if (a->group != b->group)
		return a->group - b->group;
	if (a->enabled != b->enabled)
		return b->enabled - a->enabled;
	return strcmp(a->name, b->name);
}

static void cli_ext_tool_rows_cleanup(struct cli_tool_list_row *rows, size_t count)
{
	if (!rows)
		return;
	for (size_t i = 0; i < count; i++) {
		free((char *)rows[i].name);
		free((char *)rows[i].description);
	}
	free(rows);
}

static int cli_ext_collect_tools(struct cli_application *ctx,
				 struct cli_tool_list_row **out, size_t *count_out,
				 int *enabled_count)
{
	struct cli_tool_list_row *rows;
	int count = runtime_tool_count(ctx->runtime);
	int rc = 0;

	*out = NULL;
	*count_out = 0;
	*enabled_count = 0;
	if (count <= 0)
		return 0;
	rows = calloc((size_t)count, sizeof(*rows));
	if (!rows)
		return -ENOMEM;
	for (int i = 0; i < count; i++) {
		struct tool_desc desc;
		enum tool_origin origin;
		int enabled;

		rc = runtime_tool_info_borrow(ctx->runtime, i, &desc);
		if (rc != 0)
			break;
		rc = runtime_tool_enabled(ctx->runtime, i, &enabled);
		if (rc != 0)
			break;
		rc = runtime_tool_origin(ctx->runtime, i, &origin);
		if (rc != 0)
			break;
		rows[i].group = cli_ext_origin_rank(origin);
		rows[i].enabled = enabled;
		rows[i].name = strdup(desc.name);
		rows[i].description = strdup(desc.description);
		if (!rows[i].name || !rows[i].description) {
			rc = -ENOMEM;
			break;
		}
		if (enabled)
			(*enabled_count)++;
	}
	if (rc != 0) {
		cli_ext_tool_rows_cleanup(rows, (size_t)count);
		return rc;
	}
	qsort(rows, (size_t)count, sizeof(*rows), cli_ext_tool_compare);
	*out = rows;
	*count_out = (size_t)count;
	return 0;
}

static int cli_ext_print_tools(struct cli_application *ctx)
{
	static const char *const group_names[] = {"Built-in", "Extensions", "MCP", "Dynamic",
						  "Other"};
	struct cli_tool_list_group groups[5];
	struct cli_tool_list_row *rows = NULL;
	size_t row_count = 0;
	int group_count = 0;
	int enabled_count;
	int last_group = -1;
	int rc;

	rc = cli_ext_collect_tools(ctx, &rows, &row_count, &enabled_count);
	if (rc != 0)
		return rc;
	for (size_t i = 0; i < row_count && group_count < 5; i++) {
		int group = rows[i].group;

		if (group == last_group)
			continue;
		last_group = group;
		groups[group_count].name = group_names[group];
		groups[group_count].id = group;
		group_count++;
	}
	rc = cli_tool_list_render(ctx, rows, row_count, groups, group_count, enabled_count);
	cli_ext_tool_rows_cleanup(rows, row_count);
	return rc;
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
			cli_output_printf(ctx, "  resolved_ref: %s\n", res.resolved_ref);
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
