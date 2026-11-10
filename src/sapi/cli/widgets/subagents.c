#include "sapi/cli/widgets/subagents.h"

#include "sapi/cli/models/subagents.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/widgets/transcript.h"
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
		if (segment->kind == CLI_SUBAGENTS_TOOL) {
			const struct cli_transcript_tool *tool =
				morph_array_borrow(&thread->transcript->tools, segment->tool_index);
			if (tool) {
				morph_buf_t block MORPH_CLEANUP(morph_buf_cleanup) = {0};
				rc = morph_buf_init(&block, 256);
				if (rc == 0)
					rc = cli_transcript_tool_append(&block, tool, columns,
									style.styled, details);
				if (rc == 0 && block.len) {
					if (style.trailing_newlines < 2)
						rc = morph_buf_putc(out, '\n');
					if (rc == 0)
						rc = morph_buf_append(out, block.data, block.len);
					cli_output_track_tail(&style.trailing_newlines, block.data,
							      block.len);
				}
			}
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
