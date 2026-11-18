#ifndef MORPH_CLI_COMPONENTS_TOOL_CALL_H
#define MORPH_CLI_COMPONENTS_TOOL_CALL_H

#include <nothing/tui/view.h>
struct cli_transcript_tool;
struct nt_view *cli_tool_call_add_borrow(struct nt_builder *, struct nt_view *parent,
					 const struct cli_transcript_tool *, int styled,
					 int details);
int cli_tool_call_update(struct nt_view *, const struct cli_transcript_tool *, int styled,
			 int details);

#endif
