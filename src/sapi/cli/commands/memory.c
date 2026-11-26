#include "sapi/cli/commands/memory.h"
#include <nb/err.h>
#include "runtime/runtime.h"
#include "agent/memory.h"
#include "runtime/services.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/pages/memory.h"
#include "sapi/cli/services/memory.h"

static char *cli_memory_preferences_copy(struct cli_application *ctx, int history)
{
	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!params || !cJSON_AddBoolToObject(params, "history", history))
		return NULL;

	return cli_memory_text_copy(ctx, "preference.read", params);
}

static const char *cli_memory_scope_display(enum memory_clear_scope scope)
{
	switch (scope) {
	case MEMORY_CLEAR_ALL:
		return "all";
	case MEMORY_CLEAR_FACTS:
		return "facts";
	case MEMORY_CLEAR_TASKS:
		return "past tasks";
	case MEMORY_CLEAR_TIPS:
		return "tips";
	default:
		return "unknown";
	}
}

static int cli_memory_parse_scope(const char *name, enum memory_clear_scope *scope)
{
	if (!scope)
		return NB_ERR_INVAL;

	if (!name || strcmp(name, "all") == 0) {
		*scope = MEMORY_CLEAR_ALL;
		return 0;
	}

	if (strcmp(name, "facts") == 0) {
		*scope = MEMORY_CLEAR_FACTS;
		return 0;
	}

	if (strcmp(name, "past_tasks") == 0 || strcmp(name, "episodes") == 0) {
		*scope = MEMORY_CLEAR_TASKS;
		return 0;
	}

	if (strcmp(name, "tips") == 0 || strcmp(name, "procedures") == 0 ||
	    strcmp(name, "rules") == 0) {
		*scope = MEMORY_CLEAR_TIPS;
		return 0;
	}

	return NB_ERR_INVAL;
}

static int cli_memory_on_owner(int argc, char **argv)
{
	return argc == 1 || (argc == 2 && !strcmp(argv[1], "view"));
}

static int cli_memory_command_memory(struct cli_application *ctx, int argc, char **argv)
{
	const char *sub = cli_cmd_arg_borrow(argc, argv, 1);

	if ((!sub || !strcmp(sub, "view")) && ctx->presentation_mode == CLI_PRESENT_INTERACTIVE &&
	    cli_application_tui_borrow(ctx) && isatty(STDIN_FILENO)) {
		int rc = cli_memory_page_show(ctx);

		if (rc != 0)
			CMD_ERROR(ctx, "Failed to open memory panel: %s",
				  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));

		return rc;
	}

	if (sub && !strcmp(sub, "retry")) {
		cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;
		int rc = cli_memory_request(ctx, "memory.retry", NULL, &result);

		if (rc != 0)
			CMD_ERROR(ctx, "Failed to retry memory updates: %s",
				  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		else
			CMD_OK(ctx, "Memory update retry requested.");

		return rc;
	}

	if (sub && !strcmp(sub, "jobs")) {
		char *jobs = cli_memory_text_copy(ctx, "memory.jobs", NULL);

		if (!jobs)
			return MORPH_ERR_DB;

		CMD_HEADER(ctx, "Background jobs: queued = pending; completed = finished");
		(void)cli_output_print_untrusted(ctx, jobs, MORPH_TERMINAL_TEXT_MULTILINE);
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
			return NB_ERR_INVAL;
		}

		cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;
		if (!params ||
		    !cJSON_AddStringToObject(params, "scope", scope ? scope : "personal") ||
		    !cJSON_AddStringToObject(params, "key", key) ||
		    (value ? !cJSON_AddStringToObject(params, "value", value)
			   : !cJSON_AddNullToObject(params, "value")))
			return NB_ERR_NOMEM;

		rc = cli_memory_request(ctx, "preference.update", params, &result);
		if (rc != 0) {
			CMD_ERROR(ctx, "Failed to update preference: %s",
				  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
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
			char *effective = cli_memory_preferences_copy(ctx, 0);

			(void)cli_output_print_untrusted(ctx, effective ? effective : "",
							 MORPH_TERMINAL_TEXT_MULTILINE);
			free(effective);
		}
		return 0;
	}

	if (sub && (!strcmp(sub, "history") || !strcmp(sub, "explain"))) {
		char *history = cli_memory_preferences_copy(ctx, 1);
		char *effective = cli_memory_preferences_copy(ctx, 0);

		if (!history || !effective) {
			free(history);
			free(effective);
			CMD_ERROR(ctx, "Failed to read preferences");
			return MORPH_ERR_DB;
		}

		CMD_HEADER(ctx, "Effective preferences (session > project > personal; "
				"explicit settings override legacy records)");
		(void)cli_output_print_untrusted(ctx, effective, MORPH_TERMINAL_TEXT_MULTILINE);
		CMD_HEADER(ctx, "Preference history and sources; "
				"unset removes a scope override and restores inheritance");
		(void)cli_output_print_untrusted(ctx, history, MORPH_TERMINAL_TEXT_MULTILINE);
		free(history);
		free(effective);
		return 0;
	}

	if (!sub || strcmp(sub, "show") == 0 || strcmp(sub, "view") == 0) {
		/*
		 * Pass 0 to render every stored task/change so /mem
		 * gives the user the full picture; max_past_tasks is only
		 * a hint for the React-loop context window.
		 */
		char *rendered = cli_memory_text_copy(ctx, "memory.read", NULL);
		CMD_HEADER(ctx, "memory (%s)", runtime_session_current_name_borrow(ctx->runtime));
		(void)cli_output_print_untrusted(
			ctx, rendered ? rendered : "No long-term memory stored for this session.",
			MORPH_TERMINAL_TEXT_MULTILINE);
		cli_output_printf(ctx, "\n");
		free(rendered);
		return 0;
	}

	if (strcmp(sub, "clear") == 0) {
		enum memory_clear_scope scope = MEMORY_CLEAR_ALL;
		const char *target = cli_cmd_arg_borrow(argc, argv, 2);
		if (cli_memory_parse_scope(target, &scope) != 0) {
			CMD_ERROR(ctx, "usage: /memory clear [all|facts|past_tasks|tips]");
			return NB_ERR_INVAL;
		}

		cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;
		if (!params || !cJSON_AddNumberToObject(params, "scope", scope))
			return NB_ERR_NOMEM;

		if (cli_memory_request(ctx, "memory.clear", params, &result) != 0) {
			CMD_ERROR(ctx, "failed to clear memory");
			return NB_ERR_IO;
		}

		CMD_OK(ctx,
		       "cleared %s memory for session: %s (personal/project preferences retained)",
		       cli_memory_scope_display(scope),
		       runtime_session_current_name_borrow(ctx->runtime));
		return 0;
	}

	CMD_ERROR(ctx, "usage: /memory [show|history|explain|set|unset|clear|jobs]");
	return NB_ERR_INVAL;
}

static const struct cli_command cli_memory_entries[] = {
	{"/memory", cli_memory_command_memory, "Browse memory, sources and background work",
	 "/memory [view|show|history|explain|set|unset|clear|jobs]", cli_memory_on_owner},
	{"/mem", cli_memory_command_memory, "Alias for /memory",
	 "/mem [show|clear] [all|facts|past_tasks|tips]", cli_memory_on_owner},
};

struct cli_command_group cli_memory_commands(void)
{
	return (struct cli_command_group){
		.entries = cli_memory_entries,
		.count = (int)(sizeof(cli_memory_entries) / sizeof(cli_memory_entries[0])),
		.category = "Memory",
	};
}
