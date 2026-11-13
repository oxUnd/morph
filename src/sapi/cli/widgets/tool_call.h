#ifndef MORPH_CLI_WIDGETS_TOOL_CALL_H
#define MORPH_CLI_WIDGETS_TOOL_CALL_H

#include <morph/tui/widget.h>
struct cli_transcript_tool;
struct morph_tui_view *cli_tool_call_add_borrow(struct morph_tui_builder *,
						struct morph_tui_view *parent,
						const struct cli_transcript_tool *, int styled,
						int details);
int cli_tool_call_update(struct morph_tui_view *, const struct cli_transcript_tool *, int styled,
			 int details);

#endif
