#include "sapi/cli/commands/hooks.h"
#include "util/json.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/commands/registry.h"
#include "util/sha256.h"
#include "sapi/cli/pages/hooks.h"
#include "sapi/cli/pages/setup/controls.h"

static int cli_hooks_command_hooks(struct cli_application *ctx, int argc, char **argv)
{
	struct json *rows = NULL;
	const struct json *row;
	const char *action = argc > 1 ? argv[1] : "list";
	int rc = runtime_hooks_snapshot(ctx->runtime, &rows);

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
		json_free(rows);
		return rc;
	}
	if (strcmp(action, "project") == 0) {
		const char *cwd = runtime_workdir_get(ctx->runtime);
		struct sha256_ctx sha;
		unsigned char hash[32];
		char digest[65];
		static const char hex[] = "0123456789abcdef";

		sha256_init(&sha);
		sha256_update(&sha, (const unsigned char *)cwd, strlen(cwd));
		sha256_final(&sha, hash);
		for (size_t i = 0; i < sizeof(hash); i++) {
			digest[i * 2] = hex[hash[i] >> 4];
			digest[i * 2 + 1] = hex[hash[i] & 15];
		}
		digest[64] = '\0';
		printf(ctx,
		       "Workspace: %s\nReview token: %s\n"
		       "Trust: /hooks trust-project workspace %s\n",
		       cwd, digest, digest);
	} else if (strcmp(action, "list") == 0 || strcmp(action, "check") == 0) {
		printf(ctx, "Hooks (%d); configuration inspection never executes commands.\n",
		       json_count(rows));
		json_foreach (row, rows) {
			printf(ctx, "%s  %s  %s\n  source: %s\n  matcher: %s\n",
			       json_str_or(json_member(row, "id"), NULL, ""),
			       json_str_or(json_member(row, "event"), NULL, ""),
			       json_str_or(json_member(row, "state"), NULL, ""),
			       json_str_or(json_member(row, "source"), NULL, ""),
			       json_str_or(json_member(row, "matcher"), NULL, ""));
			if (*json_str_or(json_member(row, "diagnostic"), NULL, ""))
				printf(ctx, "  unavailable: %s\n",
				       json_str_or(json_member(row, "diagnostic"), NULL, ""));
		}
		printf(ctx, "Review: /hooks review <id>\nWorkspace review: /hooks project\n");
		if (json_count(rows) > 0) {
			const char *digest = json_str_or(
				json_member(json_at(rows, 0), "review_digest"), NULL, "");
			printf(ctx,
			       "Trust all: /hooks trust-all workspace %s\n"
			       "Reject all: /hooks deny-all workspace %s\n",
			       digest, digest);
		}
	} else if (strcmp(action, "review") == 0 && argc == 3) {
		rc = -ENOENT;
		json_foreach (row, rows) {
			if (strcmp(json_str_or(json_member(row, "id"), NULL, ""), argv[2]) != 0)
				continue;
			printf(ctx,
			       "Source: %s\nCwd: %s\nState: %s\n"
			       "Previous definition:\n%s\nCurrent definition:\n%s\n"
			       "Trust covers this definition, not mutable script contents.\n"
			       "Trust: /hooks trust %s %s\n",
			       json_str_or(json_member(row, "source"), NULL, ""),
			       json_str_or(json_member(row, "cwd"), NULL, ""),
			       json_str_or(json_member(row, "state"), NULL, ""),
			       json_str_or(json_member(row, "previous_definition"), NULL, ""),
			       json_str_or(json_member(row, "definition"), NULL, ""),
			       json_str_or(json_member(row, "id"), NULL, ""),
			       json_str_or(json_member(row, "digest"), NULL, ""));
			if (*json_str_or(json_member(row, "diagnostic"), NULL, ""))
				printf(ctx, "Unavailable: %s\n",
				       json_str_or(json_member(row, "diagnostic"), NULL, ""));
			const struct json *last = json_member_ci(row, "last_run");
			if (last) {
				char *text = json_stringify_pretty(last);
				if (text) {
					printf(ctx, "Last execution:\n%s\n", text);
					free(text);
				}
			}
			rc = 0;
		}
	} else if (argc == 4) {
		rc = runtime_hooks_manage(ctx->runtime, action, argv[2], argv[3]);
		if (rc == 0)
			printf(ctx, "Hook action completed: %s\n", action);
	} else {
		printf(ctx, "Usage: /hooks [list|check|project|review-all|review <id>|"
			    "trust|enable|disable|revoke <id> <digest>]\n");
		rc = -EINVAL;
	}
	json_free(rows);
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
