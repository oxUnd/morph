#ifndef MORPH_CLI_PRESENTATION_AGENT_TOOLS_H
#define MORPH_CLI_PRESENTATION_AGENT_TOOLS_H

#include "util/buf.h"
struct cli_transcript_tool;

int cli_agent_tools_matches(const char *name);
int cli_agent_tools_analytics_only(const char *name);
int cli_agent_tools_row(const struct cli_transcript_tool *tool, morph_buf_t *out, int columns,
			int styled);
int cli_agent_tools_preview(const struct cli_transcript_tool *tool, morph_buf_t *out);

#endif
