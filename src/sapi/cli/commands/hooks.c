#include "sapi/cli/commands/hooks.h"
#include "nb/err.h"
#include "sapi/cli/presentation/output.h"
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "cJSON.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/pages/hooks.h"
#include "sapi/cli/pages/setup/controls.h"

static const char *cli_hooks_hook_field(const cJSON *row, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(row, key);

	return cJSON_IsString(value) ? value->valuestring : "";
}

static int cli_hooks_command_hooks(struct cli_application *ctx, int argc, char **argv)
{
	cJSON *rows = NULL;
	const cJSON *row;
	const char *action = argc > 1 ? argv[1] : "list";
	int rc = runtime_hooks_snapshot_copy(ctx->runtime, &rows);

	if (rc != 0)
		return rc;
	if (ctx->presentation_mode != CLI_PRESENT_EVENTS_JSON &&
	    cli_setup_controls_available(stdin, stdout) &&
	    ((argc <= 2 && strcmp(action, "list") == 0) ||
	     (argc == 2 && strcmp(action, "review-all") == 0) ||
	     (argc == 3 && strcmp(action, "review") == 0))) {
		rc = strcmp(action, "review-all") == 0
			     ? cli_hooks_review_all(ctx, &rows)
			     : cli_hooks_ui(ctx, &rows, argc == 3 ? argv[2] : NULL);
		cJSON_Delete(rows);
		return rc;
	}
	if (strcmp(action, "project") == 0) {
		const char *cwd = runtime_workdir_borrow(ctx->runtime);
		char digest[65];

		runtime_hooks_workspace_token(ctx->runtime, digest);
		cli_output_printf(ctx,
		       "Workspace: %s\nReview token: %s\n"
		       "Trust: /hooks trust-project workspace %s\n",
		       cwd, digest, digest);
	} else if (strcmp(action, "list") == 0 || strcmp(action, "check") == 0) {
		cli_output_printf(ctx, "Hooks (%d); configuration inspection never executes commands.\n",
		       cJSON_GetArraySize(rows));
		cJSON_ArrayForEach (row, rows) {
			cli_output_printf(ctx, "%s  %s  %s\n  source: %s\n  matcher: %s\n",
			       cli_hooks_hook_field(row, "id"), cli_hooks_hook_field(row, "event"),
			       cli_hooks_hook_field(row, "state"),
			       cli_hooks_hook_field(row, "source"),
			       cli_hooks_hook_field(row, "matcher"));
			if (*cli_hooks_hook_field(row, "diagnostic"))
				cli_output_printf(ctx, "  unavailable: %s\n",
				       cli_hooks_hook_field(row, "diagnostic"));
		}
		cli_output_printf(ctx, "Review: /hooks review <id>\nWorkspace review: /hooks project\n");
		if (cJSON_GetArraySize(rows) > 0) {
			const char *digest =
				cli_hooks_hook_field(cJSON_GetArrayItem(rows, 0), "review_digest");
			cli_output_printf(ctx,
			       "Trust all: /hooks trust-all workspace %s\n"
			       "Reject all: /hooks deny-all workspace %s\n",
			       digest, digest);
		}
	} else if (strcmp(action, "review") == 0 && argc == 3) {
		rc = NB_ERR_NOT_FOUND;
		cJSON_ArrayForEach (row, rows) {
			if (strcmp(cli_hooks_hook_field(row, "id"), argv[2]) != 0)
				continue;
			cli_output_printf(ctx,
			       "Source: %s\nCwd: %s\nState: %s\n"
			       "Previous definition:\n%s\nCurrent definition:\n%s\n"
			       "Trust covers this definition, not mutable script contents.\n"
			       "Trust: /hooks trust %s %s\n",
			       cli_hooks_hook_field(row, "source"),
			       cli_hooks_hook_field(row, "cwd"), cli_hooks_hook_field(row, "state"),
			       cli_hooks_hook_field(row, "previous_definition"),
			       cli_hooks_hook_field(row, "definition"),
			       cli_hooks_hook_field(row, "id"),
			       cli_hooks_hook_field(row, "digest"));
			if (*cli_hooks_hook_field(row, "diagnostic"))
				cli_output_printf(ctx, "Unavailable: %s\n",
				       cli_hooks_hook_field(row, "diagnostic"));
			const cJSON *last = cJSON_GetObjectItem(row, "last_run");
			if (last) {
				char *text = cJSON_Print(last);
				if (text) {
					cli_output_printf(ctx, "Last execution:\n%s\n", text);
					free(text);
				}
			}
			rc = 0;
		}
	} else if (argc == 4) {
		rc = runtime_hooks_manage(ctx->runtime, action, argv[2], argv[3]);
		if (rc == 0)
			cli_output_printf(ctx, "Hook action completed: %s\n", action);
	} else {
		cli_output_printf(ctx, "Usage: /hooks [list|check|project|review-all|review <id>|"
			    "trust|enable|disable|revoke <id> <digest>]\n");
		rc = NB_ERR_INVAL;
	}
	cJSON_Delete(rows);
	return rc;
}

struct cli_command_group cli_hook_commands(void)
{
	static const struct cli_command commands[] = {
		{"/hooks", cli_hooks_command_hooks, "Review and manage lifecycle hooks",
		 "/hooks [list|check|project|review <id>|<action> <id> <digest>]"},
	};

	return (struct cli_command_group){commands, 1, "Runtime"};
}
