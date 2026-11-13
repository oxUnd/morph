#ifndef MORPH_CLI_WIDGETS_TRANSCRIPT_H
#define MORPH_CLI_WIDGETS_TRANSCRIPT_H

#include "nb/buf.h"
#include "nb/cleanup.h"

struct cli_application;
struct cli_transcript_model;
struct cli_transcript_tool;

int cli_transcript_live_text(const struct cli_transcript_model *model, nb_buf *text, int columns,
			     int styled);
int cli_transcript_live_body(const struct cli_transcript_model *model, nb_buf *text, int columns,
			     int styled);
int cli_transcript_render_details(struct cli_application *ctx,
				  const struct cli_transcript_model *model, nb_buf *output);
int cli_transcript_tool_append(nb_buf *out, const struct cli_transcript_tool *tool, int columns,
			       int styled, int details);
void cli_transcript_render_tool(struct cli_application *ctx, const struct cli_transcript_tool *tool,
				int compact);
void cli_transcript_render_tool_details(struct cli_application *ctx,
					const struct cli_transcript_tool *tool, int args);
void cli_transcript_render_payload(struct cli_application *ctx, const char *text);
void cli_transcript_render_stream(struct cli_application *ctx,
				  const struct cli_transcript_tool *tool, size_t offset);

#endif
