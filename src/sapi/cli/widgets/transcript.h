#ifndef MORPH_CLI_WIDGETS_TRANSCRIPT_H
#define MORPH_CLI_WIDGETS_TRANSCRIPT_H

#include "event/event.h"
#include "util/buf.h"

struct cli_application;

int cli_transcript_capture_begin(struct cli_application *ctx);
int cli_transcript_event(struct cli_application *ctx, const struct morph_event *ev);
int cli_transcript_reasoning_append(struct cli_application *ctx, const char *text, size_t len,
				    int begin);
void cli_transcript_reset(struct cli_application *ctx);
void cli_transcript_toggle(struct cli_application *ctx);
void cli_transcript_view_suspend(struct cli_application *ctx);
void cli_transcript_view_resume(struct cli_application *ctx);
void cli_transcript_view_render(struct cli_application *ctx, int scroll);
void cli_transcript_finish(struct cli_application *ctx);
int cli_transcript_live_body(struct cli_application *ctx, morph_buf_t *text, int styled);
int cli_transcript_live_text(struct cli_application *ctx, morph_buf_t *text, int styled);

#endif
