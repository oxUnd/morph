#include "sapi/cli/widgets/feed_block.h"
#include "nb/err.h"

#include "sapi/cli/application.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/widgets/agents.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/widgets/plan.h"
#include "sapi/cli/widgets/notice.h"
#include "nb/cleanup.h"
#include <errno.h>

int cli_feed_block_leading_rows(struct morph_tui_view_insets margin, int trailing_newlines)
{
	return cli_output_block_gap_margin(margin, trailing_newlines);
}

int cli_feed_block_append(nb_buf *out, const struct cli_feed_block *block,
			  struct cli_feed_block_style *style)
{
	if (!out || !block || !style)
		return NB_ERR_INVAL;
	nb_buf body NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&body), nb_buf_reserve(&body, 128) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	switch (block->kind) {
	case CLI_FEED_BLOCK_REASONING: {
		struct cli_message_style message = {
			.columns = style->columns, .styled = style->styled,
			.trailing_newlines = 2, .reasoning_expanded = 1,
		};
		rc = cli_messages_append(&body, CLI_MESSAGE_REASONING,
			block->content.reasoning, &message);
		break;
	}
	case CLI_FEED_BLOCK_TOOL:
		rc = cli_transcript_tool_append(&body, block->content.tool,
			style->columns, style->styled, block->details);
		break;
	case CLI_FEED_BLOCK_AGENT_RECEIPT:
		rc = cli_agents_receipt_append(&body, block->content.agent.id,
			block->content.agent.status, style->columns, style->styled);
		break;
	case CLI_FEED_BLOCK_PLAN: {
		struct cli_plan_style plan = {
			.columns = style->columns, .styled = style->styled, .status_heading = 1,
		};
		rc = cli_plan_append(&body, block->content.plan, &plan);
		break;
	}
	case CLI_FEED_BLOCK_NOTICE:
		rc = cli_notice_append(&body, block->content.notice, style->columns, style->styled);
		break;
	default:
		return NB_ERR_INVAL;
	}
	if (rc != 0 || !body.len)
		return rc;
	struct morph_tui_view_insets margin = block->margin;

	if (block->join_previous)
		margin = (struct morph_tui_view_insets){0};
	else if (!margin.top && !margin.right && !margin.bottom && !margin.left)
		margin.top = 1;
	return cli_output_block_append(out, nb_buf_cstr(&body), margin, &style->trailing_newlines);
}

int cli_feed_block_write(struct cli_application *ctx, const struct cli_feed_block *block)
{
	if (!ctx)
		return NB_ERR_INVAL;
	if (cli_chat_active(ctx))
		return cli_chat_block(ctx, block);
	nb_buf document NB_CLEANUP(nb_buf_cleanup) = {0};
	struct cli_feed_block_style style = {
		.columns = cli_feed_columns(ctx), .styled = cli_output_color_enabled(ctx),
		.trailing_newlines = ctx->output.capture && !ctx->output.capture_history
			? 0 : ctx->output.trailing_newlines,
	};
	int rc = (nb_buf_init(&document), nb_buf_reserve(&document, 128) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = cli_feed_block_append(&document, block, &style);
	if (rc == 0 && document.len)
		rc = cli_output_printf(ctx, "%s", nb_buf_cstr(&document));
	return rc < 0 ? rc : 0;
}

int cli_feed_notice_write(struct cli_application *ctx, enum cli_notice_status status,
			  const char *title, const char *detail, const char *footer)
{
	struct cli_notice notice = {
		.status = status, .title = title, .detail = detail, .footer = footer,
	};
	struct cli_feed_block block = {
		.kind = CLI_FEED_BLOCK_NOTICE, .content.notice = &notice,
	};
	return cli_feed_block_write(ctx, &block);
}
