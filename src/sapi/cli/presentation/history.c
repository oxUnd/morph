#include "sapi/cli/presentation/history.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/services/transcript.h"
#include "session.h"
#include "util/error.h"
#include "util/strmap.h"
#include "util/json.h"
#include <string.h>

static void cli_history_shell_argument(morph_buf_t *command, const char *value)
{
	static const char safe[] =
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_@%+=:,./-";

	(void)morph_buf_putc(command, ' ');
	if (value[0] && strspn(value, safe) == strlen(value)) {
		(void)morph_buf_puts(command, value);
		return;
	}
	(void)morph_buf_putc(command, '\'');
	for (const char *p = value; *p; p++) {
		if (*p == '\'')
			(void)morph_buf_puts(command, "'\\''");
		else
			(void)morph_buf_putc(command, *p);
	}
	(void)morph_buf_putc(command, '\'');
}

void cli_history_render_resume_hint(struct cli_application *ctx, const char *session_ref,
				    const char *config_path, const char *workdir)
{
	morph_buf_t arguments;
	int color = cli_output_color_enabled(ctx);
	const char *command_style = color ? ANSI_BOLD CLI_COMMAND_COLOR : "";
	const char *dim = color ? ANSI_DIM : "";
	const char *reset = color ? ANSI_RESET : "";

	if (morph_buf_init(&arguments, 128) != 0)
		return;
	(void)morph_buf_puts(&arguments, command_style);
	(void)morph_buf_puts(&arguments, "resume");
	if (color)
		(void)morph_buf_puts(&arguments, ANSI_RESET ANSI_BOLD ANSI_CYAN);
	cli_history_shell_argument(&arguments, session_ref);
	(void)morph_buf_puts(&arguments, reset);
	(void)morph_buf_puts(&arguments, dim);
	if (config_path && config_path[0]) {
		(void)morph_buf_puts(&arguments, " -c");
		cli_history_shell_argument(&arguments, config_path);
	}
	if (workdir && workdir[0]) {
		(void)morph_buf_puts(&arguments, " -w");
		cli_history_shell_argument(&arguments, workdir);
	}
	(void)morph_buf_puts(&arguments, reset);
	if (!arguments.failed)
		cli_output_printf(ctx,
				  "\n%sTo resume this session, run:%s\n"
				  "  %smorph %s\n  %smorphctl %s\n",
				  dim, reset, command_style, morph_buf_cstr(&arguments),
				  command_style, morph_buf_cstr(&arguments));
	morph_buf_cleanup(&arguments);
}

static void cli_history_answer(struct cli_application *ctx, const char *text)
{
	if (!text || !text[0])
		return;
	cli_markdown_render_final(ctx, text);
	cli_markdown_finish_final(ctx);
}

static int cli_history_assistant(struct cli_application *ctx, const struct model_history_item *item,
				 const morph_strmap_t *results)
{
	struct json *payload =
		item->payload_json ? json_parse_prefix(item->payload_json, NULL, NULL) : NULL;
	const char *reasoning = json_str_or(json_member(payload, "reasoning_content"), NULL, "");
	const struct json *call;
	int rc = 0;

	if (reasoning[0])
		rc = cli_transcript_reasoning_append(ctx, reasoning, strlen(reasoning), 1);
	cli_history_answer(ctx, item->content);
	json_foreach (call, json_member(payload, "calls")) {
		if (rc)
			break;
		const char *id = json_str_or(json_member(call, "tool_call_id"), NULL, "");
		struct model_history_item *result = morph_strmap_get(results, id);

		rc = cli_transcript_restore_tool(ctx, call, result);
	}
	json_free(payload);
	return rc;
}

int cli_history_render_session(struct cli_application *ctx, struct model_history_item *items,
			       const struct message *messages)
{
	morph_strmap_t results;
	const char *turn = NULL;
	int rc = morph_strmap_init(&results, 32);

	if (rc)
		return rc;
	/* Include inactive records: compression affects model context, not scrollback. */
	for (struct model_history_item *item = items; item; item = item->next) {
		if (strcmp(item->kind, "tool_result") || !item->tool_call_id)
			continue;
		rc = morph_strmap_set(&results, item->tool_call_id, item);
		if (rc)
			break;
	}
	for (const struct model_history_item *item = items; !rc && item; item = item->next) {
		if (strcmp(item->kind, "user_message") == 0) {
			if (!turn || !turn[0] || !item->turn_id || strcmp(turn, item->turn_id))
				cli_transcript_reset(ctx);
			turn = item->turn_id;
			cli_presentation_user_input(ctx, item->content);
		} else if (strcmp(item->kind, "assistant_message") == 0 ||
			   strcmp(item->kind, "assistant_tool_calls") == 0) {
			rc = cli_history_assistant(ctx, item, &results);
		}
	}
	/* Older/imported sessions can contain messages without canonical model history. */
	for (const struct message *message = messages; !rc && message; message = message->next) {
		if (strcmp(message->role, "user") == 0)
			cli_presentation_user_input(ctx, message->content);
		else if (strcmp(message->role, "assistant") == 0)
			cli_history_answer(ctx, message->content);
	}
	cli_transcript_finish(ctx);
	morph_strmap_cleanup(&results);
	return rc;
}
