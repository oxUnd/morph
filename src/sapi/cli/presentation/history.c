#include "sapi/cli/presentation/history.h"
#include "sapi/cli/application.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/services/transcript.h"
#include "session.h"
#include "errors.h"
#include "nb/map.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "cJSON.h"
#include <string.h>

static void cli_history_shell_argument(nb_buf *command, const char *value)
{
	static const char safe[] =
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_@%+=:,./-";

	(void)nb_buf_append_char(command, ' ');
	if (value[0] && strspn(value, safe) == strlen(value)) {
		(void)nb_buf_append_cstr(command, value);
		return;
	}
	(void)nb_buf_append_char(command, '\'');
	for (const char *p = value; *p; p++) {
		if (*p == '\'')
			(void)nb_buf_append_cstr(command, "'\\''");
		else
			(void)nb_buf_append_char(command, *p);
	}
	(void)nb_buf_append_char(command, '\'');
}

void cli_history_render_resume_hint(struct cli_application *ctx, const char *session_ref,
				     const char *config_path, const char *workdir)
{
	nb_buf arguments NB_CLEANUP(nb_buf_cleanup) = {0};
	int color = cli_output_color_enabled(ctx);
	const char *command_style = color ? ANSI_BOLD CLI_COMMAND_COLOR : "";
	const char *dim = color ? ANSI_DIM : "";
	const char *reset = color ? ANSI_RESET : "";

	if ((nb_buf_init(&arguments), nb_buf_reserve(&arguments, 128) ? 0 : NB_ERR_NOMEM) != 0)
		return;
	(void)nb_buf_append_cstr(&arguments, command_style);
	(void)nb_buf_append_cstr(&arguments, "resume");
	if (color)
		(void)nb_buf_append_cstr(&arguments, ANSI_RESET ANSI_BOLD ANSI_CYAN);
	cli_history_shell_argument(&arguments, session_ref);
	(void)nb_buf_append_cstr(&arguments, reset);
	(void)nb_buf_append_cstr(&arguments, dim);
	if (config_path && config_path[0]) {
		(void)nb_buf_append_cstr(&arguments, " -c");
		cli_history_shell_argument(&arguments, config_path);
	}
	if (workdir && workdir[0]) {
		(void)nb_buf_append_cstr(&arguments, " -w");
		cli_history_shell_argument(&arguments, workdir);
	}
	(void)nb_buf_append_cstr(&arguments, reset);
	if (!nb_buf_status(&arguments))
		cli_output_printf(ctx,
				  "\n%sTo resume this session, run:%s\n"
				  "  %smorph %s\n  %smorphctl %s\n",
				  dim, reset, command_style, nb_buf_cstr(&arguments), command_style,
				  nb_buf_cstr(&arguments));
	nb_buf_cleanup(&arguments);
}

static const char *cli_history_string(const cJSON *object, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);

	return cJSON_IsString(value) ? value->valuestring : "";
}

static void cli_history_answer(struct cli_application *ctx, const char *text)
{
	if (!text || !text[0])
		return;
	if (cli_chat_active(ctx)) {
		(void)cli_chat_message(ctx, CLI_MESSAGE_ASSISTANT, text);
		return;
	}
	cli_markdown_render_final(ctx, text);
	cli_markdown_finish_final(ctx);
}

static int cli_history_assistant(struct cli_application *ctx, const struct model_history_item *item,
				 const nb_map *results)
{
	cJSON *payload = item->payload_json ? cJSON_Parse(item->payload_json) : NULL;
	const char *reasoning = cli_history_string(payload, "reasoning_content");
	const cJSON *call;
	int rc = 0;

	if (reasoning[0])
		rc = cli_transcript_reasoning_append(ctx, reasoning, strlen(reasoning), 1);
	if (!rc && reasoning[0] && cli_chat_active(ctx))
		rc = cli_chat_message(ctx, CLI_MESSAGE_REASONING, reasoning);
	cli_history_answer(ctx, item->content);
	cJSON_ArrayForEach(call, cJSON_GetObjectItemCaseSensitive(payload, "calls")) {
		if (rc)
			break;
		const char *id = cli_history_string(call, "tool_call_id");
		struct model_history_item *result = nb_map_get_cstr(results, id);

		rc = cli_transcript_restore_tool(ctx, call, result);
	}
	cJSON_Delete(payload);
	return rc;
}

int cli_history_render_session(struct cli_application *ctx, struct model_history_item *items,
			       const struct message *messages)
{
	nb_map *results NB_CLEANUP(nb_map_cleanup) = nb_map_new_with_cap(32);
	const char *turn = NULL;
	int rc = results ? 0 : NB_ERR_NOMEM;

	if (rc)
		return rc;
	/* Include inactive records: compression affects model context, not scrollback. */
	for (struct model_history_item *item = items; item; item = item->next) {
		if (strcmp(item->kind, "tool_result") || !item->tool_call_id)
			continue;
		if (!nb_map_put_cstr(results, item->tool_call_id, item, NULL)) {
			rc = NB_ERR_NOMEM;
			break;
		}
	}
	for (const struct model_history_item *item = items; !rc && item; item = item->next) {
		if (strcmp(item->kind, "user_message") == 0) {
			if (!turn || !turn[0] || !item->turn_id || strcmp(turn, item->turn_id))
				cli_transcript_reset(ctx);
			turn = item->turn_id;
			cJSON *payload NB_CLEANUP(cJSON_DeleteOwner) =
				item->payload_json ? cJSON_Parse(item->payload_json) : NULL;

			if (strcmp(cli_history_string(payload, "source"), "hook") == 0)
				cli_presentation_hook_prompt(ctx, item->content);
			else
				cli_presentation_user_input(ctx, item->content);
		} else if (strcmp(item->kind, "assistant_message") == 0 ||
			   strcmp(item->kind, "assistant_tool_calls") == 0) {
			rc = cli_history_assistant(ctx, item, results);
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
	return rc;
}
