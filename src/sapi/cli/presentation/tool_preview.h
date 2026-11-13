#ifndef MORPH_CLI_PRESENTATION_TOOL_PREVIEW_H
#define MORPH_CLI_PRESENTATION_TOOL_PREVIEW_H

#include "nb/buf.h"
#include "nb/cleanup.h"

struct cli_transcript_tool;

/* Capture a call-time diff. Historical replay and widget rendering never read
 * the current workspace to reconstruct an old patch. */
int cli_tool_patch_preview(const struct cli_transcript_tool *tool, const char *workdir,
			   nb_buf *preview);

#endif
