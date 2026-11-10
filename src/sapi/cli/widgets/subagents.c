#include "sapi/cli/widgets/subagents.h"

#include "sapi/cli/models/subagents.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/widgets/agents.h"
#include "sapi/cli/widgets/transcript.h"
#include "util/cleanup.h"
#include <errno.h>

int cli_subagents_transcript(struct cli_application *ctx, const struct cli_subagents_thread *thread,
			     morph_buf_t *out, int columns)
{
	if (!ctx || !thread || !out)
		return -EINVAL;
	morph_buf_t text MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_init(&text, 1024);
	if (rc == 0)
		rc = cli_output_capture_begin(ctx, out);
	if (rc != 0)
		return rc;
	const struct cli_subagents_segment *segment;
	morph_array_foreach (segment, &thread->segments, struct cli_subagents_segment) {
		if (segment->kind == CLI_SUBAGENTS_TOOL) {
			const struct cli_transcript_tool *tool =
				morph_array_borrow(&thread->transcript->tools, segment->tool_index);
			if (tool) {
				cli_transcript_render_tool(ctx, tool, 0);
				cli_transcript_render_tool_details(ctx, tool, 1);
				cli_output_printf(ctx, "\n");
			}
		} else {
			morph_buf_reset(&text);
			const char *role = segment->kind == CLI_SUBAGENTS_USER	      ? "User"
					   : segment->kind == CLI_SUBAGENTS_REASONING ? "Reasoning"
										      : "Assistant";
			rc = cli_agents_detail(&text, role, morph_buf_cstr_borrow(&segment->text),
					       columns, 0);
			if (rc != 0)
				break;
			cli_output_printf(ctx, "%s\n", morph_buf_cstr_borrow(&text));
		}
	}
	cli_output_capture_end(ctx);
	return rc != 0 ? rc : out->failed;
}
