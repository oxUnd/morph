#include "sapi/cli/widgets/subagents.h"

#include "sapi/cli/models/subagents.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/widgets/feed_block.h"
#include "util/json.h"
#include "util/cleanup.h"
#include <errno.h>

int cli_subagents_transcript(struct cli_application *ctx, const struct cli_subagents_thread *thread,
			     morph_buf_t *out, int columns, int details)
{
	if (!ctx || !thread || !out)
		return -EINVAL;
	struct cli_message_style style = {
		.columns = columns,
		.styled = cli_output_color_enabled(ctx),
		.ultra = ctx->composer.ultra,
		.reasoning_expanded = details,
	};
	cli_output_track_tail(&style.trailing_newlines, morph_buf_cstr_borrow(out), out->len);
	int rc = 0;
	const struct cli_subagents_segment *segment;
	morph_array_foreach (segment, &thread->segments, struct cli_subagents_segment) {
		if (segment->kind == CLI_SUBAGENTS_TOOL || segment->kind == CLI_SUBAGENTS_PLAN) {
			struct cli_feed_block_style feed = {
				.columns = columns, .styled = style.styled,
				.trailing_newlines = style.trailing_newlines,
			};
			struct cli_feed_block block = {.details = details};
			cJSON *plan MORPH_CLEANUP(morph_json_cleanup) = NULL;
			if (segment->kind == CLI_SUBAGENTS_PLAN) {
				plan = cJSON_Parse(morph_buf_cstr_borrow(&segment->text));
				if (!plan)
					continue;
				block.kind = CLI_FEED_BLOCK_PLAN;
				block.content.plan = plan;
			} else {
				block.kind = CLI_FEED_BLOCK_TOOL;
				block.content.tool = morph_array_borrow(&thread->transcript->tools,
					segment->tool_index);
				if (!block.content.tool)
					continue;
			}
			rc = cli_feed_block_append(out, &block, &feed);
			style.trailing_newlines = feed.trailing_newlines;
		} else {
			enum cli_message_kind kind =
				segment->kind == CLI_SUBAGENTS_USER	   ? CLI_MESSAGE_USER
				: segment->kind == CLI_SUBAGENTS_REASONING ? CLI_MESSAGE_REASONING
									   : CLI_MESSAGE_ASSISTANT;
			rc = cli_messages_append(out, kind, morph_buf_cstr_borrow(&segment->text),
						 &style);
		}
		if (rc != 0)
			break;
	}
	return rc != 0 ? rc : out->failed;
}
