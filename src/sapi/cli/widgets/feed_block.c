#include "sapi/cli/widgets/feed_block.h"

#include "sapi/cli/application.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/widgets/agents.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/transcript.h"
#include "util/cleanup.h"
#include <errno.h>

int cli_feed_block_leading_rows(int trailing_newlines)
{
	return trailing_newlines < 2;
}

int cli_feed_block_append(morph_buf_t *out, const struct cli_feed_block *block,
			 struct cli_feed_block_style *style)
{
	if (!out || !block || !style)
		return -EINVAL;
	morph_buf_t body MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_init(&body, 128);
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
			style->columns, style->styled, 0);
		break;
	case CLI_FEED_BLOCK_AGENT_RECEIPT:
		rc = cli_agents_receipt_append(&body, block->content.agent.id,
			block->content.agent.status, style->columns, style->styled);
		break;
	default:
		return -EINVAL;
	}
	if (rc != 0 || !body.len)
		return rc;
	size_t start = out->len;
	if (!block->join_previous && cli_feed_block_leading_rows(style->trailing_newlines))
		rc = morph_buf_putc(out, '\n');
	if (rc == 0)
		rc = morph_buf_append(out, body.data, body.len);
	if (rc == 0)
		cli_output_track_tail(&style->trailing_newlines, out->data + start, out->len - start);
	return rc;
}

int cli_feed_block_write(struct cli_application *ctx, const struct cli_feed_block *block)
{
	if (!ctx)
		return -EINVAL;
	morph_buf_t document MORPH_CLEANUP(morph_buf_cleanup) = {0};
	struct cli_feed_block_style style = {
		.columns = cli_feed_columns(ctx), .styled = cli_output_color_enabled(ctx),
		.trailing_newlines = ctx->output.capture && !ctx->output.capture_history
			? 0 : ctx->output.trailing_newlines,
	};
	int rc = morph_buf_init(&document, 128);
	if (rc == 0)
		rc = cli_feed_block_append(&document, block, &style);
	if (rc == 0 && document.len)
		rc = cli_output_printf(ctx, "%s", morph_buf_cstr_borrow(&document));
	return rc < 0 ? rc : 0;
}
