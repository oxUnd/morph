#include "cli/commands/registry.h"

static int cmd_skill(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = cli_cmd_arg(argc, argv, 1);
	if (sub && strcmp(sub, "list") == 0) {
		CMD_HEADER("available skills (%d)", ctx->skills->count);
		for (int i = 0; i < ctx->skills->count; i++) {
			struct skill_entry *e = &ctx->skills->entries[i];
			const char *marker = e->activated ? ANSI_GREEN " *" ANSI_RESET : "";
			printf("  %-25s %s%s\n", e->fm.name, e->fm.description,
			       marker);
		}
		if (ctx->skills->count == 0)
			printf("  (none — install skills to ~/.morph/skills/ or ~/.agents/skills/)\n");
		else
			printf("  " ANSI_DIM "(%s* = activated)" ANSI_RESET "\n", ANSI_GREEN);
		return 0;
	}
	if (sub && strcmp(sub, "info") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /skill info <name>");
			return -EINVAL;
		}
		struct skill_entry *e = skill_lookup(ctx->skills, name);
		if (!e) {
			CMD_ERROR("skill not found: %s", name);
			return -ENOENT;
		}
		printf("  %-15s %s\n", "Name", e->fm.name);
		printf("  %-15s %s\n", "Description", e->fm.description);
		printf("  %-15s %s\n", "Directory", e->skill_dir);
		printf("  %-15s %s\n", "Enabled", e->enabled ? "yes" : "no");
		printf("  %-15s %s\n", "Activated", e->activated ? "yes" : "no");
		if (e->fm.license[0])
			printf("  %-15s %s\n", "License", e->fm.license);
		if (e->fm.compatibility[0])
			printf("  %-15s %s\n", "Compatibility", e->fm.compatibility);
		if (e->fm.allowed_tools[0])
			printf("  %-15s %s\n", "Allowed tools", e->fm.allowed_tools);
		for (int i = 0; i < e->fm.metadata_count; i++)
			printf("  %-15s %s = %s\n", (i == 0) ? "Metadata" : "",
			       e->fm.metadata[i].key, e->fm.metadata[i].value);
		return 0;
	}
	if (sub && strcmp(sub, "activate") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /skill activate <name>");
			return -EINVAL;
		}
		struct skill_entry *e = skill_lookup(ctx->skills, name);
		if (!e) {
			CMD_ERROR("skill not found: %s", name);
			return -ENOENT;
		}
		int rc = skill_activate(e);
		if (rc == 0 && e->activated)
			CMD_OK("skill '%s' activated", name);
		else if (rc < 0)
			CMD_ERROR("failed to activate skill '%s': %s", name, morph_strerror(rc));
		else
			CMD_OK("skill '%s' already activated", name);
		return rc < 0 ? rc : 0;
	}
	if (sub && strcmp(sub, "deactivate") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /skill deactivate <name>");
			return -EINVAL;
		}
		struct skill_entry *e = skill_lookup(ctx->skills, name);
		if (!e) {
			CMD_ERROR("skill not found: %s", name);
			return -ENOENT;
		}
		skill_deactivate(e);
		CMD_OK("skill '%s' deactivated", name);
		return 0;
	}
	CMD_HEADER("available skills (%d)", ctx->skills->count);
	for (int i = 0; i < ctx->skills->count; i++) {
		struct skill_entry *e = &ctx->skills->entries[i];
		const char *marker = e->activated ? ANSI_GREEN " *" ANSI_RESET : "";
		printf("  %-25s %s%s\n", e->fm.name, e->fm.description, marker);
	}
	if (ctx->skills->count == 0)
		printf("  (none — install skills to ~/.morph/skills/ or ~/.agents/skills/)\n");
	return 0;
}


static const struct cli_command skill_commands[] = {
	{ "/skill",   cmd_skill,   "List or manage skills",             "/skill list" },
	{ "/sk",      cmd_skill,   "Alias for /skill",                  "/sk list" },
};

int cli_register_skill_commands(void)
{
	return cli_command_register_many(skill_commands,
		(int)(sizeof(skill_commands) / sizeof(skill_commands[0])));
}
