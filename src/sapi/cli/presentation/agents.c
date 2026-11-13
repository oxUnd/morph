#include "sapi/cli/presentation/agents.h"
#include "sapi/cli/models/agents.h"

#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/presenter_state.h"
#include "sapi/cli/widgets/agents.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/feed_block.h"
#include "util/cleanup.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

void cli_presentation_agents_flush(struct cli_application *ctx)
{
	struct cli_agents_receipt *receipt;
	int joined = 0;

	if (!ctx || !ctx->agents.receipts.nelts)
		return;
	cli_feed_history_begin(ctx);
	morph_array_foreach (receipt, &ctx->agents.receipts, struct cli_agents_receipt) {
		struct cli_feed_block block = {
			.kind = CLI_FEED_BLOCK_AGENT_RECEIPT,
			.join_previous = joined,
			.content.agent = {.id = receipt->id, .status = receipt->status},
		};

		(void)cli_feed_block_write(ctx, &block);
		joined = 1;
	}
	cli_agents_model_receipts_clear(&ctx->agents);
	cli_feed_history_end(ctx);
}

int cli_presentation_agents_event(struct cli_application *ctx,
				  const struct morph_event *event)
{
	const char *id;
	const char *status;
	int completed;
	int cancelled;
	enum agent_status outcome;

	if (!ctx || !event || !event->name ||
	    (strcmp(event->name, "agent.started") && strcmp(event->name, "agent.updated") &&
	     strcmp(event->name, "agent.completed")))
		return 0;
	if (cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
		    event->data, "parent_session_id")) != (double)ctx->agents.session_id)
		return 1;
	if (strcmp(event->name, "agent.completed"))
		return 1;
	id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event->data, "agent_id"));
	status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event->data, "status"));
	completed = status && !strcmp(status, "completed");
	cancelled = status && (!strcmp(status, "interrupted") || !strcmp(status, "cancelled"));
	outcome = completed ? AGENT_COMPLETED : cancelled ? AGENT_CANCELLED : AGENT_FAILED;
	if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE && ctx->turn_active &&
	    ctx->presenter && !ctx->presenter->final_rendered &&
	    (ctx->presenter->markdown_stream || ctx->presenter->native_content_started ||
	     (ctx->presenter->event_stream_has_delta &&
	      !ctx->presenter->event_stream_complete))) {
		/* Keep parent answer streams contiguous. Snapshots still update live. */
		int rc = cli_agents_model_receipt_add(&ctx->agents, id, outcome);

		if (rc != 0)
			return rc;
	} else {
		struct cli_feed_block block = {
			.kind = CLI_FEED_BLOCK_AGENT_RECEIPT,
			.content.agent = {.id = id, .status = outcome},
		};

		cli_feed_history_begin(ctx);
		if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
			(void)cli_feed_block_write(ctx, &block);
		} else {
			morph_buf_t line MORPH_CLEANUP(morph_buf_cleanup) = {0};

			if (morph_buf_init(&line, 128) == 0 &&
			    cli_agents_receipt_append(&line, id, outcome, BUFSIZ,
					cli_output_color_enabled(ctx)) == 0)
				cli_output_printf(ctx, "%s", morph_buf_cstr_borrow(&line));
		}
		cli_feed_history_end(ctx);
	}
	cli_feed_queue_changed(ctx);
	return 1;
}
