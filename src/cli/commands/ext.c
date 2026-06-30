#include "cli/commands/registry.h"

static int cmd_ext(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = cli_cmd_arg(argc, argv, 1);
	if (sub && strcmp(sub, "list") == 0) {
		CMD_HEADER("registered tools (%d)", ctx->tools.count);
		for (int i = 0; i < ctx->tools.count; i++) {
			printf("  %-15s %s\n",
			       ctx->tools.entries[i].desc.name,
			       ctx->tools.entries[i].desc.desc);
		}
		if (ctx->tools.count == 0)
			printf("  (none)\n");
		return 0;
	}
	if (sub && strcmp(sub, "info") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /ext info <name>");
			return -EINVAL;
		}
		struct tool_entry *e = tool_lookup(&ctx->tools, name);
		if (!e) {
			CMD_ERROR("tool not found: %s", name);
			return -ENOENT;
		}
		printf("  %-15s %s\n", "Name", e->desc.name);
		printf("  %-15s %s\n", "Description", e->desc.desc);
		if (e->desc.args_spec[0])
			printf("  %-15s %s\n", "Args spec", e->desc.args_spec);
		return 0;
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
		opts.install_dir = ctx->config.ext.dir[0] ?
			ctx->config.ext.dir : "~/.morph/exts";
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
	/* /ext alone → show tools */
	CMD_HEADER("registered tools (%d)", ctx->tools.count);
	for (int i = 0; i < ctx->tools.count; i++) {
		printf("  %-15s %s\n",
		       ctx->tools.entries[i].desc.name,
		       ctx->tools.entries[i].desc.desc);
	}
	if (ctx->tools.count == 0)
		printf("  (none)\n");
	return 0;
}


static const struct cli_command ext_commands[] = {
	{ "/ext",     cmd_ext,     "List or manage tools and exts",     "/ext list" },
	{ "/x",       cmd_ext,     "Alias for /ext",                    "/x list" },
};

int cli_register_ext_commands(void)
{
	return cli_command_register_many(ext_commands,
		(int)(sizeof(ext_commands) / sizeof(ext_commands[0])));
}
