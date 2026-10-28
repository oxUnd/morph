#include "sapi/cli/commands/registry.h"
#include "util/sha256.h"
#include "sapi/cli/hooks_ui.h"
#include "sapi/cli/setup_ui.h"

static const char *hook_field(const cJSON *row, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(row, key);

	return cJSON_IsString(value) ? value->valuestring : "";
}

static int cmd_hooks(struct cli_context *ctx, int argc, char **argv)
{
	cJSON *rows = NULL;
	const cJSON *row;
	const char *action = argc > 1 ? argv[1] : "list";
	int rc = runtime_hooks_snapshot(ctx->runtime, &rows);

	if (rc != 0)
		return rc;
	if (ctx->presentation_mode != CLI_PRESENT_EVENTS_JSON &&
	    cli_setup_ui_available(stdin, stdout) &&
	    ((argc <= 2 && strcmp(action, "list") == 0) ||
	     (argc == 2 && strcmp(action, "review-all") == 0) ||
	     (argc == 3 && strcmp(action, "review") == 0))) {
		rc = strcmp(action, "review-all") == 0 ? cli_hooks_review_all(ctx, &rows) :
			cli_hooks_ui(ctx, &rows, argc == 3 ? argv[2] : NULL);
		cJSON_Delete(rows);
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
		       cJSON_GetArraySize(rows));
		cJSON_ArrayForEach(row, rows)
		{
			printf(ctx, "%s  %s  %s\n  source: %s\n  matcher: %s\n",
			       hook_field(row, "id"), hook_field(row, "event"),
			       hook_field(row, "state"), hook_field(row, "source"),
			       hook_field(row, "matcher"));
		}
		printf(ctx, "Review: /hooks review <id>\nWorkspace review: /hooks project\n");
		if (cJSON_GetArraySize(rows) > 0) {
			const char *digest = hook_field(cJSON_GetArrayItem(rows, 0), "review_digest");
			printf(ctx,
			       "Trust all: /hooks trust-all workspace %s\n"
			       "Reject all: /hooks deny-all workspace %s\n",
			       digest, digest);
		}
	} else if (strcmp(action, "review") == 0 && argc == 3) {
		rc = -ENOENT;
		cJSON_ArrayForEach(row, rows)
		{
			if (strcmp(hook_field(row, "id"), argv[2]) != 0)
				continue;
			printf(ctx,
			       "Source: %s\nCwd: %s\nState: %s\n"
			       "Previous definition:\n%s\nCurrent definition:\n%s\n"
			       "Trust covers this definition, not mutable script contents.\n"
			       "Trust: /hooks trust %s %s\n",
			       hook_field(row, "source"), hook_field(row, "cwd"),
			       hook_field(row, "state"), hook_field(row, "previous_definition"),
			       hook_field(row, "definition"), hook_field(row, "id"),
			       hook_field(row, "digest"));
			const cJSON *last = cJSON_GetObjectItem(row, "last_run");
			if (last) {
				char *text = cJSON_Print(last);
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
	cJSON_Delete(rows);
	return rc;
}

struct cli_command_group cli_hook_commands(void)
{
	static const struct cli_command commands[] = {
	    {"/hooks", cmd_hooks, "Review and manage lifecycle hooks",
	     "/hooks [list|check|project|review <id>|<action> <id> <digest>]"},
	};

	return (struct cli_command_group){commands, 1, "Runtime"};
}
