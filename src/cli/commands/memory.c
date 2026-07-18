#include "cli/commands/registry.h"

static const char *memory_scope_display(enum memory_clear_scope scope)
{
	switch (scope) {
	case MEMORY_CLEAR_ALL:
		return "all";
	case MEMORY_CLEAR_FACTS:
		return "facts";
	case MEMORY_CLEAR_EPISODES:
		return "episodes";
	case MEMORY_CLEAR_PROCEDURES:
		return "procedures";
	default:
		return "unknown";
	}
}

static int memory_parse_scope(const char *name, enum memory_clear_scope *scope)
{
	if (!scope)
		return -EINVAL;
	if (!name || strcmp(name, "all") == 0) {
		*scope = MEMORY_CLEAR_ALL;
		return 0;
	}
	if (strcmp(name, "facts") == 0) {
		*scope = MEMORY_CLEAR_FACTS;
		return 0;
	}
	if (strcmp(name, "episodes") == 0) {
		*scope = MEMORY_CLEAR_EPISODES;
		return 0;
	}
	if (strcmp(name, "procedures") == 0 ||
	    strcmp(name, "rules") == 0) {
		*scope = MEMORY_CLEAR_PROCEDURES;
		return 0;
	}
	return -EINVAL;
}

static int cmd_memory(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = cli_cmd_arg(argc, argv, 1);

	if (!sub || strcmp(sub, "show") == 0 || strcmp(sub, "view") == 0) {
		/*
		 * Pass 0 to render every stored episode/change so /mem
		 * gives the user the full picture; max_episodes is only
		 * a hint for the React-loop context window.
		 */
		char *rendered = runtime_memory_render_current(ctx->runtime, 0);
		CMD_HEADER("memory (%s)", runtime_session_current_name(ctx->runtime));
		printf("%s\n", rendered ? rendered :
		       "No long-term memory stored for this session.");
		free(rendered);
		return 0;
	}

	if (strcmp(sub, "clear") == 0) {
		enum memory_clear_scope scope = MEMORY_CLEAR_ALL;
		const char *target = cli_cmd_arg(argc, argv, 2);
		if (memory_parse_scope(target, &scope) != 0) {
			CMD_ERROR("usage: /memory clear [all|facts|episodes|procedures]");
			return -EINVAL;
		}
		if (runtime_memory_clear_current(ctx->runtime, scope) != 0) {
			CMD_ERROR("failed to clear memory");
			return -EIO;
		}
		CMD_OK("cleared %s memory for session: %s",
		       memory_scope_display(scope), runtime_session_current_name(ctx->runtime));
		return 0;
	}

	CMD_ERROR("usage: /memory [show|clear] [all|facts|episodes|procedures]");
	return -EINVAL;
}


static const struct cli_command memory_commands[] = {
	{ "/memory",  cmd_memory,  "Show or clear long-term memory",    "/memory [show|clear] [all|facts|episodes|procedures]" },
	{ "/mem",     cmd_memory,  "Alias for /memory",                 "/mem [show|clear] [all|facts|episodes|procedures]" },
};

int cli_register_memory_commands(void)
{
	return cli_command_register_many(memory_commands,
		(int)(sizeof(memory_commands) / sizeof(memory_commands[0])));
}
