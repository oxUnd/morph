#include "sapi/cli/commands/registry.h"

static int cmd_skill(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = cli_cmd_arg(argc, argv, 1);
	if (sub && strcmp(sub, "list") == 0) {
		int count = runtime_skill_count(ctx->runtime);
		CMD_HEADER("available skills (%d)", count);
		for (int i = 0; i < count; i++) {
			struct skill_entry entry;
			(void)runtime_skill_info(ctx->runtime, i, &entry);
			struct skill_entry *e = &entry;
			const char *marker = e->activated ? ANSI_GREEN " *" ANSI_RESET : "";
			printf("  %-25s %s%s\n", e->fm.name, e->fm.description,
			       marker);
		}
		if (count == 0)
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
		struct skill_entry entry;
		if (runtime_skill_find(ctx->runtime, name, &entry) != 0) {
			CMD_ERROR("skill not found: %s", name);
			return -ENOENT;
		}
		printf("  %-15s %s\n", "Name", entry.fm.name);
		printf("  %-15s %s\n", "Description", entry.fm.description);
		printf("  %-15s %s\n", "Directory", entry.skill_dir);
		printf("  %-15s %s\n", "Enabled", entry.enabled ? "yes" : "no");
		printf("  %-15s %s\n", "Activated", entry.activated ? "yes" : "no");
		if (entry.fm.license[0])
			printf("  %-15s %s\n", "License", entry.fm.license);
		if (entry.fm.compatibility[0])
			printf("  %-15s %s\n", "Compatibility", entry.fm.compatibility);
		if (entry.fm.allowed_tools[0])
			printf("  %-15s %s\n", "Allowed tools", entry.fm.allowed_tools);
		for (int i = 0; i < entry.fm.metadata_count; i++)
			printf("  %-15s %s = %s\n", (i == 0) ? "Metadata" : "",
			       entry.fm.metadata[i].key, entry.fm.metadata[i].value);
		return 0;
	}
	if (sub && strcmp(sub, "activate") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /skill activate <name>");
			return -EINVAL;
		}
		struct skill_entry entry;
		if (runtime_skill_find(ctx->runtime, name, &entry) != 0) {
			CMD_ERROR("skill not found: %s", name);
			return -ENOENT;
		}
		int changed = 0;
		int rc = runtime_skill_set_active(ctx->runtime, name, 1, &changed);
		if (rc == 0 && changed)
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
		struct skill_entry entry;
		if (runtime_skill_find(ctx->runtime, name, &entry) != 0) {
			CMD_ERROR("skill not found: %s", name);
			return -ENOENT;
		}
		(void)runtime_skill_set_active(ctx->runtime, name, 0, NULL);
		CMD_OK("skill '%s' deactivated", name);
		return 0;
	}
	int count = runtime_skill_count(ctx->runtime);
	CMD_HEADER("available skills (%d)", count);
	for (int i = 0; i < count; i++) {
		struct skill_entry entry;
		(void)runtime_skill_info(ctx->runtime, i, &entry);
		struct skill_entry *e = &entry;
		const char *marker = e->activated ? ANSI_GREEN " *" ANSI_RESET : "";
		printf("  %-25s %s%s\n", e->fm.name, e->fm.description, marker);
	}
	if (count == 0)
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
					 (int)(sizeof(skill_commands) /
					 sizeof(skill_commands[0])),
					 "Skills");
}
