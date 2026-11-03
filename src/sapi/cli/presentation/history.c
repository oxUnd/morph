#include "sapi/cli/presentation/history.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/widgets/transcript.h"
#include "session.h"
#include "util/error.h"
#include "util/strmap.h"
#include "cJSON.h"
#include <string.h>

static const char *cli_history_string(const cJSON *object, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);

	return cJSON_IsString(value) ? value->valuestring : "";
}

static void cli_history_answer(struct cli_application *ctx, const char *text)
{
	if (!text || !text[0])
		return;
	cli_markdown_render_final(ctx, text);
	cli_markdown_finish_final(ctx);
}

static int cli_history_assistant(struct cli_application *ctx,
				 const struct model_history_item *item,
				 const morph_strmap_t *results)
{
	cJSON *payload = item->payload_json ? cJSON_Parse(item->payload_json) : NULL;
	const char *reasoning = cli_history_string(payload, "reasoning_content");
	const cJSON *call;
	int rc = 0;

	if (reasoning[0])
		rc = cli_transcript_reasoning_append(ctx, reasoning, strlen(reasoning), 1);
	cli_history_answer(ctx, item->content);
	cJSON_ArrayForEach(call, cJSON_GetObjectItemCaseSensitive(payload, "calls")) {
		if (rc)
			break;
		const char *id = cli_history_string(call, "tool_call_id");
		struct model_history_item *result = morph_strmap_get(results, id);

		rc = cli_transcript_restore_tool(ctx, call, result);
	}
	cJSON_Delete(payload);
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
