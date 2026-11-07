#include "sapi/cli/commands/memory.h"
#include "agent/memory.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/commands/registry.h"

static const char *cli_memory_scope_display(enum memory_clear_scope scope)
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

static int cli_memory_parse_scope(const char *name, enum memory_clear_scope *scope)
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
	if (strcmp(name, "procedures") == 0 || strcmp(name, "rules") == 0) {
		*scope = MEMORY_CLEAR_PROCEDURES;
		return 0;
	}
	return -EINVAL;
}

static int cli_memory_command_memory(struct cli_application *ctx, int argc, char **argv)
{
	const char *sub = cli_cmd_arg_borrow(argc, argv, 1);

	if (sub && !strcmp(sub, "jobs")) {
		char *jobs = runtime_memory_background_render_copy(ctx->runtime);

		if (!jobs)
			return MORPH_ERR_DB;
		CMD_HEADER(ctx, "Background jobs: queued = pending; completed = finished");
		(void)cli_output_print_untrusted(ctx, jobs, UTF8_TERMINAL_TEXT_MULTILINE);
		free(jobs);
		return 0;
	}

	if (sub && (!strcmp(sub, "set") || !strcmp(sub, "unset"))) {
		int setting = !strcmp(sub, "set");
		const char *key = cli_cmd_arg_borrow(argc, argv, 2);
		const char *value = setting ? cli_cmd_arg_borrow(argc, argv, 3) : NULL;
		const char *scope = cli_cmd_arg_borrow(argc, argv, setting ? 4 : 3);
		int rc;

		if (!key || (setting && !value) || argc > (setting ? 5 : 4)) {
			CMD_ERROR(ctx, "/memory set <key> <value> [personal|project|session]"
				       "; /memory unset <key> [personal|project|session]");
			return -EINVAL;
		}
		rc = runtime_preference_set(ctx->runtime, scope ? scope : "personal", key, value);
		if (rc != 0) {
			CMD_ERROR(ctx, "Failed to update preference: %s", morph_strerror(rc));
			return rc;
		}
		if (setting)
			CMD_OK(ctx, "Preference saved (%s); effective from the next model request.",
			       scope ? scope : "personal");
		else
			CMD_OK(ctx,
			       "Preference unset (%s); inheritance restored for the next model "
			       "request.",
			       scope ? scope : "personal");
		{
			char *effective = runtime_preferences_render_copy(ctx->runtime, 0);

			(void)cli_output_print_untrusted(ctx, effective ? effective : "",
							 UTF8_TERMINAL_TEXT_MULTILINE);
			free(effective);
		}
		return 0;
	}
	if (sub && (!strcmp(sub, "history") || !strcmp(sub, "explain"))) {
		char *history = runtime_preferences_render_copy(ctx->runtime, 1);
		char *effective = runtime_preferences_render_copy(ctx->runtime, 0);

		if (!history || !effective) {
			free(history);
			free(effective);
			CMD_ERROR(ctx, "Failed to read preferences");
			return MORPH_ERR_DB;
		}
		CMD_HEADER(ctx, "Effective preferences (session > project > personal; "
				"explicit settings override legacy records)");
		(void)cli_output_print_untrusted(ctx, effective, UTF8_TERMINAL_TEXT_MULTILINE);
		CMD_HEADER(ctx, "Preference history and sources; "
				"unset removes a scope override and restores inheritance");
		(void)cli_output_print_untrusted(ctx, history, UTF8_TERMINAL_TEXT_MULTILINE);
		free(history);
		free(effective);
		return 0;
	}

	if (!sub || strcmp(sub, "show") == 0 || strcmp(sub, "view") == 0) {
		/*
		 * Pass 0 to render every stored episode/change so /mem
		 * gives the user the full picture; max_episodes is only
		 * a hint for the React-loop context window.
		 */
		char *rendered = runtime_memory_render_current_copy(ctx->runtime, 0);
		CMD_HEADER(ctx, "memory (%s)", runtime_session_current_name_borrow(ctx->runtime));
		(void)cli_output_print_untrusted(
			ctx, rendered ? rendered : "No long-term memory stored for this session.",
			UTF8_TERMINAL_TEXT_MULTILINE);
		printf(ctx, "\n");
		free(rendered);
		return 0;
	}

	if (strcmp(sub, "clear") == 0) {
		enum memory_clear_scope scope = MEMORY_CLEAR_ALL;
		const char *target = cli_cmd_arg_borrow(argc, argv, 2);
		if (cli_memory_parse_scope(target, &scope) != 0) {
			CMD_ERROR(ctx, "usage: /memory clear [all|facts|episodes|procedures]");
			return -EINVAL;
		}
		if (runtime_memory_clear_current(ctx->runtime, scope) != 0) {
			CMD_ERROR(ctx, "failed to clear memory");
			return -EIO;
		}
		CMD_OK(ctx,
		       "cleared %s memory for session: %s (personal/project preferences retained)",
		       cli_memory_scope_display(scope),
		       runtime_session_current_name_borrow(ctx->runtime));
		return 0;
	}

	CMD_ERROR(ctx, "usage: /memory [show|history|explain|set|unset|clear|jobs]");
	return -EINVAL;
}

static const struct cli_command cli_memory_entries[] = {
	{"/memory", cli_memory_command_memory, "Show or clear long-term memory",
	 "/memory [show|history|explain|set|unset|clear|jobs]"},
	{"/mem", cli_memory_command_memory, "Alias for /memory",
	 "/mem [show|clear] [all|facts|episodes|procedures]"},
};

struct cli_command_group cli_memory_commands(void)
{
	return (struct cli_command_group){
		.entries = cli_memory_entries,
		.count = (int)(sizeof(cli_memory_entries) / sizeof(cli_memory_entries[0])),
		.category = "Memory",
	};
}
