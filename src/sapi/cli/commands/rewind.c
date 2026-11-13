#include "sapi/cli/commands/rewind.h"
#include "nb/err.h"
#include "sapi/cli/presentation/output.h"
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/pages/rewind.h"
#include "sapi/cli/pages/setup/controls.h"
#include "sapi/cli/presentation/rewind.h"
#include "sapi/cli/services/command_job.h"
#include "sapi/cli/services/session_restore.h"
#include "sapi/cli/widgets/list.h"
#include "runtime/checkpoint.h"
#include "nb/cleanup.h"
#include "cJSON.h"

static void cli_rewind_help(struct cli_application *ctx)
{
	cli_output_printf(ctx, "Usage: /rewind [list|preview <id>|code <id>|conversation <id>|both <id>]\n"
		    "Choose a turn checkpoint, then restore code, conversation, or both.\n"
		    "Code rewind covers apply_patch edits; shell and external changes are not "
		    "captured.\n");
}

static const char *cli_rewind_string(const cJSON *object, const char *key)
{
	const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
	return value ? value : "";
}

static int cli_rewind_restore(struct cli_application *ctx, int64_t id,
			      enum runtime_rewind_mode mode)
{
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc = runtime_checkpoint_restore(ctx->runtime, id, mode, &report);
	int conversation_restored =
		cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(report, "conversation_restored"));
	int display_rc = 0;

	if (conversation_restored || (!rc && (mode & RUNTIME_REWIND_CONVERSATION)))
		display_rc = cli_session_history_restore(ctx);

	if (rc) {
		const char *reload = cli_rewind_string(report, "history_reload_error");
		const char *receipt = cli_rewind_string(report, "history_receipt_error");
		if (conversation_restored && *reload)
			CMD_ERROR(ctx, "rewind applied; runtime history reload failed: %s", reload);
		else if (*receipt)
			CMD_ERROR(ctx, "rewind applied; history note could not be saved: %s",
				  receipt);
		else
			CMD_ERROR(ctx, "rewind failed: %s",
				  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(report, "code_restored")))
			cli_output_printf(ctx, "  Code was restored before this error.\n");
		if (conversation_restored)
			cli_output_printf(ctx, "  Conversation was restored before this error.\n");
		if (display_rc)
			cli_output_printf(ctx, "  History display refresh failed: %s\n",
					  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
						     display_rc));
		cli_rewind_report_print(ctx, report);
		return rc;
	}
	if (display_rc) {
		CMD_ERROR(ctx, "rewind completed; history display failed: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, display_rc));
		cli_rewind_report_print(ctx, report);
		return display_rc;
	}
	CMD_OK(ctx, "rewound %s to checkpoint %lld",
	       mode == RUNTIME_REWIND_CODE	     ? "code"
	       : mode == RUNTIME_REWIND_CONVERSATION ? "conversation"
						     : "code and conversation",
	       (long long)id);
	if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(report, "cleanup_incomplete")))
		cli_rewind_report_print(ctx, report);
	return 0;
}

static int cli_rewind_id(const char *text, int64_t *id)
{
	char *end;
	long long value;

	if (!text || !*text || *text < '0' || *text > '9')
		return NB_ERR_INVAL;
	errno = 0;
	value = strtoll(text, &end, 10);
	if (errno || *end || value <= 0 || value > INT64_MAX)
		return NB_ERR_INVAL;
	*id = (int64_t)value;
	return 0;
}

static int cli_rewind_list(struct cli_application *ctx, int interactive)
{
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc = runtime_checkpoint_list_copy(ctx->runtime, &report);
	if (rc)
		return rc;
	cJSON *entries = cJSON_GetObjectItemCaseSensitive(report, "checkpoints");
	if (interactive && cJSON_GetArraySize(entries) > 0 &&
	    cli_setup_controls_available(stdin, stdout))
		return cli_rewind_show_take(ctx, &report, cli_rewind_restore);
	CMD_HEADER(ctx, "rewind checkpoints");
	const cJSON *entry;
	cJSON_ArrayForEach (entry, entries) {
		char prompt[BUFSIZ];
		cli_list_compact_text(prompt, sizeof(prompt), cli_rewind_string(entry, "prompt"));
		const cJSON *files = cJSON_GetObjectItemCaseSensitive(entry, "files");
		int count = cJSON_IsArray(files)    ? cJSON_GetArraySize(files)
			    : cJSON_IsNumber(files) ? files->valueint
						    : 0;
		cli_output_printf(ctx, "  %lld  %s (%d files)\n",
		       (long long)cJSON_GetNumberValue(
			       cJSON_GetObjectItemCaseSensitive(entry, "id")),
		       prompt, count);
	}
	if (!cJSON_GetArraySize(entries))
		cli_output_printf(ctx, "  No checkpoints in this session.\n");
	if (interactive)
		cli_rewind_help(ctx);
	return 0;
}

static int cli_rewind_command(struct cli_application *ctx, int argc, char **argv)
{
	if (argc == 2 &&
	    (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		cli_rewind_help(ctx);
		return 0;
	}
	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;
	if (ctx->turn_active || (ctx->input_job && ctx->input_job->active)) {
		CMD_ERROR(ctx, "wait for the active turn before rewinding");
		return NB_ERR_BUSY;
	}
	int rc;
	if (argc == 1 || (argc == 2 && !strcmp(argv[1], "list"))) {
		rc = cli_rewind_list(ctx, argc == 1 && ctx->presentation_mode ==
							       CLI_PRESENT_INTERACTIVE);
	} else {
		int64_t id;
		if (argc != 3 || cli_rewind_id(argv[2], &id) != 0) {
			cli_rewind_help(ctx);
			return NB_ERR_INVAL;
		}
		if (!strcmp(argv[1], "preview")) {
			cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = NULL;
			rc = runtime_checkpoint_preview_copy(ctx->runtime, id, &report);
			if (report) {
				char prompt[BUFSIZ];
				cli_list_compact_text(prompt, sizeof(prompt),
						      cli_rewind_string(report, "prompt"));
				CMD_HEADER(ctx, "checkpoint %lld · %s", (long long)id, prompt);
				cli_rewind_report_print(ctx, report);
			}
		} else {
			enum runtime_rewind_mode mode =
				!strcmp(argv[1], "code")	   ? RUNTIME_REWIND_CODE
				: !strcmp(argv[1], "conversation") ? RUNTIME_REWIND_CONVERSATION
				: !strcmp(argv[1], "both")	   ? RUNTIME_REWIND_BOTH
								   : 0;
			if (!mode) {
				cli_rewind_help(ctx);
				return NB_ERR_INVAL;
			}
			return cli_rewind_restore(ctx, id, mode);
		}
	}
	if (rc)
		CMD_ERROR(ctx, "rewind failed: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	return rc;
}

struct cli_command_group cli_rewind_commands(void)
{
	static const struct cli_command commands[] = {
		{"/rewind", cli_rewind_command, "Restore a turn checkpoint",
		 "/rewind [list|preview <id>|code <id>|conversation <id>|both <id>]"},
	};
	return (struct cli_command_group){commands, (int)(sizeof(commands) / sizeof(commands[0])),
					  "Session"};
}
