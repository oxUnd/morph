#ifndef MORPH_CLI_WIDGETS_TRANSCRIPT_H
#define MORPH_CLI_WIDGETS_TRANSCRIPT_H

#include "event/event.h"
#include "util/buf.h"

struct cli_application;
struct cJSON;
struct model_history_item;

int cli_transcript_event(struct cli_application *ctx, const struct morph_event *ev);
int cli_transcript_restore_tool(struct cli_application *ctx, const struct cJSON *call,
				const struct model_history_item *result);
int cli_transcript_reasoning_append(struct cli_application *ctx, const char *text, size_t len,
				    int begin);
void cli_transcript_reset(struct cli_application *ctx);
int cli_transcript_render_details(struct cli_application *ctx, morph_buf_t *output);
void cli_transcript_finish(struct cli_application *ctx);
int cli_transcript_live_body(struct cli_application *ctx, morph_buf_t *text, int styled);
int cli_transcript_live_text(struct cli_application *ctx, morph_buf_t *text, int styled);

#endif
