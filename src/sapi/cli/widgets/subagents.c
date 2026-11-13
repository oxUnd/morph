#include "sapi/cli/widgets/subagents.h"
#include "nb/err.h"

#include "sapi/cli/models/subagents.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/widgets/feed_block.h"
#include "cJSON.h"
#include "nb/cleanup.h"
#include <errno.h>

int cli_subagents_transcript(struct cli_application *ctx, const struct cli_subagents_thread *thread,
			     nb_buf *out, int columns, int details)
{
	if (!ctx || !thread || !out)
		return NB_ERR_INVAL;
	struct cli_message_style style = {
		.columns = columns,
		.styled = cli_output_color_enabled(ctx),
		.ultra = ctx->composer.ultra,
		.reasoning_expanded = details,
	};
	cli_output_track_tail(&style.trailing_newlines, nb_buf_cstr(out), out->len);
	int rc = 0;
	const struct cli_subagents_segment *segment;
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&thread->segments) &&
	     (segment = (struct cli_subagents_segment *)nb_vec_at(&thread->segments, _i_));
	     _i_++) {
		if (segment->kind == CLI_SUBAGENTS_TOOL || segment->kind == CLI_SUBAGENTS_PLAN) {
			struct cli_feed_block_style feed = {
				.columns = columns, .styled = style.styled,
				.trailing_newlines = style.trailing_newlines,
			};
			struct cli_feed_block block = {.details = details};
			cJSON *plan NB_CLEANUP(cJSON_DeleteOwner) = NULL;
			if (segment->kind == CLI_SUBAGENTS_PLAN) {
				plan = cJSON_Parse(nb_buf_cstr(&segment->text));
				if (!plan)
					continue;
				block.kind = CLI_FEED_BLOCK_PLAN;
				block.content.plan = plan;
			} else {
				block.kind = CLI_FEED_BLOCK_TOOL;
				block.content.tool =
					nb_vec_at(&thread->transcript->tools, segment->tool_index);
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
			rc = cli_messages_append(out, kind, nb_buf_cstr(&segment->text), &style);
		}
		if (rc != 0)
			break;
	}
	return rc != 0 ? rc : nb_buf_status(out);
}
