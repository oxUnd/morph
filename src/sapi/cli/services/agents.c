#include "sapi/cli/services/agents.h"

#include "sapi/cli/application.h"
#include "sapi/cli/widgets/agents.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/feed_block.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/presenter_state.h"
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "util/cleanup.h"

struct cli_agents_snapshot {
	struct agent_info *items;
	int count;
};

struct cli_agents_receipt {
	char *id;
	enum agent_status status;
};

int cli_agents_receipts_init(struct cli_application *ctx)
{
	struct cli_agents_receipt receipt = {0};
	return morph_array_init(&ctx->presenter->agent_receipts, 4, sizeof(receipt));
}

static void cli_agents_receipts_clear(struct cli_application *ctx)
{
	struct cli_agents_receipt *receipt;
	morph_array_foreach (receipt, &ctx->presenter->agent_receipts, struct cli_agents_receipt)
		free(receipt->id);
	morph_array_clear(&ctx->presenter->agent_receipts);
}

void cli_agents_receipts_cleanup(struct cli_application *ctx)
{
	cli_agents_receipts_clear(ctx);
	morph_array_cleanup(&ctx->presenter->agent_receipts);
}

static void cli_agents_snapshot_cleanup(struct cli_agents_snapshot *snapshot)
{
	runtime_agent_list_free(snapshot->items, snapshot->count);
}

int cli_agents_visible(const struct cli_application *ctx)
{
	if (!ctx || !ctx->runtime)
		return 0;
	if (ctx->composer.ultra && ctx->turn_active)
		return 1;
	struct cli_agents_snapshot snapshot MORPH_CLEANUP(cli_agents_snapshot_cleanup) = {0};
	int active = 0;
	int64_t session = 0;
	(void)runtime_session_current_id(ctx->runtime, &session);
	if (runtime_agent_activity(ctx->runtime, session, &snapshot.items, &snapshot.count) == 0) {
		for (int i = 0; i < snapshot.count; i++)
			active |= snapshot.items[i].status == AGENT_RUNNING ||
				  snapshot.items[i].status == AGENT_PENDING;
	}
	return active;
}

int cli_agents_render(struct cli_application *ctx, morph_buf_t *out, morph_buf_t *summary,
		      int columns, int maximum, int styled)
{
	struct cli_agents_snapshot snapshot MORPH_CLEANUP(cli_agents_snapshot_cleanup) = {0};
	int rows = 0;
	int64_t session = 0;
	(void)runtime_session_current_id(ctx->runtime, &session);
	if (runtime_agent_activity(ctx->runtime, session, &snapshot.items, &snapshot.count) == 0) {
		(void)cli_agents_summary(summary, snapshot.items, snapshot.count, columns);
		rows = cli_agents_rows(out, snapshot.items, snapshot.count, columns, maximum,
				       styled);
	}
	return rows;
}

void cli_agents_flush_receipts(struct cli_application *ctx)
{
	if (!ctx || !ctx->presenter || !ctx->presenter->agent_receipts.nelts)
		return;
	cli_feed_history_begin(ctx);
	struct cli_agents_receipt *receipt;
	int joined = 0;
	morph_array_foreach (receipt, &ctx->presenter->agent_receipts, struct cli_agents_receipt) {
		struct cli_feed_block block = {
			.kind = CLI_FEED_BLOCK_AGENT_RECEIPT, .join_previous = joined,
			.content.agent = {.id = receipt->id, .status = receipt->status},
		};
		(void)cli_feed_block_write(ctx, &block);
		joined = 1;
	}
	cli_agents_receipts_clear(ctx);
	cli_feed_history_end(ctx);
}

int cli_agents_event(struct cli_application *ctx, const struct morph_event *event)
{
	if (!event->name ||
	    (strcmp(event->name, "agent.started") && strcmp(event->name, "agent.updated") &&
	     strcmp(event->name, "agent.completed")))
		return 0;
	int64_t session = 0;
	(void)runtime_session_current_id(ctx->runtime, &session);
	if (cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
		    event->data, "parent_session_id")) != (double)session)
		return 1;
	if (!strcmp(event->name, "agent.completed")) {
		const char *id = cJSON_GetStringValue(
			cJSON_GetObjectItemCaseSensitive(event->data, "agent_id"));
		const char *status = cJSON_GetStringValue(
			cJSON_GetObjectItemCaseSensitive(event->data, "status"));
		int completed = status && !strcmp(status, "completed");
		int cancelled = status && (!strcmp(status, "interrupted") ||
					   !strcmp(status, "cancelled"));
		enum agent_status outcome = completed ? AGENT_COMPLETED
			: cancelled ? AGENT_CANCELLED : AGENT_FAILED;
		if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE && ctx->turn_active &&
		    ctx->presenter && !ctx->presenter->final_rendered &&
		    (ctx->presenter->markdown_stream || ctx->presenter->native_content_started ||
		     (ctx->presenter->event_stream_has_delta &&
		      !ctx->presenter->event_stream_complete))) {
			/* Keep parent answer streams contiguous. Snapshots still update live. */
			char *owned MORPH_CLEANUP(morph_charp_cleanup) = strdup(id ? id : "agent");
			if (!owned)
				return -ENOMEM;
			struct cli_agents_receipt *receipt =
				morph_array_push_borrow(&ctx->presenter->agent_receipts);
			if (!receipt)
				return -ENOMEM;
			receipt->id = morph_charp_steal(&owned);
			receipt->status = outcome;
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
	}
	cli_feed_queue_changed(ctx);
	return 1;
}
