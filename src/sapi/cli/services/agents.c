#include "sapi/cli/services/agents.h"

#include "sapi/cli/application.h"
#include "sapi/cli/widgets/agents.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/list.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/presenter_state.h"
#include "sapi/cli/presentation/style.h"
#include <string.h>
#include "util/cleanup.h"

struct cli_agents_snapshot {
	struct agent_info *items;
	int count;
};

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
	if (!ctx || !ctx->presenter || !ctx->presenter->agent_receipts.len)
		return;
	cli_feed_history_begin(ctx);
	cli_output_printf(ctx, "%s%s", ctx->output.trailing_newlines < 2 ? "\n" : "",
			  morph_buf_cstr_borrow(&ctx->presenter->agent_receipts));
	morph_buf_reset(&ctx->presenter->agent_receipts);
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
		char safe[BUFSIZ];
		const char *label = id ? id : "agent";
		cli_list_compact_text(safe, sizeof(safe), label);
		const char *action = status && !strcmp(status, "completed")	? "Finished"
				     : status && !strcmp(status, "interrupted") ? "Interrupted"
										: "Failed";
		if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE && ctx->turn_active &&
		    ctx->presenter && !ctx->presenter->final_rendered &&
		    (ctx->presenter->markdown_stream || ctx->presenter->native_content_started ||
		     (ctx->presenter->event_stream_has_delta &&
		      !ctx->presenter->event_stream_complete))) {
			/* Keep parent answer streams contiguous. Snapshots still update live. */
			int rc = morph_buf_printf(&ctx->presenter->agent_receipts,
						  ANSI_DIM "• " ANSI_RESET "%s " CLI_COMMAND_COLOR
							   "%s" ANSI_RESET "\n",
						  action, safe);
			if (rc != 0)
				return rc;
		} else {
			cli_feed_history_begin(ctx);
			cli_output_printf(ctx,
					  ANSI_DIM "• " ANSI_RESET "%s " CLI_COMMAND_COLOR
						   "%s" ANSI_RESET "\n",
					  action, safe);
			cli_feed_history_end(ctx);
		}
	}
	cli_feed_queue_changed(ctx);
	return 1;
}
