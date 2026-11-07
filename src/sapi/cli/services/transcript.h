#ifndef MORPH_CLI_SERVICES_TRANSCRIPT_H
#define MORPH_CLI_SERVICES_TRANSCRIPT_H

#include <stddef.h>

struct cli_application;
struct morph_event;
struct cJSON;
struct model_history_item;

/* Coordinates model updates and their presentation on the UI owner thread. */
int cli_transcript_event(struct cli_application *ctx, const struct morph_event *event);
int cli_transcript_restore_tool(struct cli_application *ctx, const struct cJSON *call,
				const struct model_history_item *result);
int cli_transcript_reasoning_append(struct cli_application *ctx, const char *text, size_t length,
				    int begin);
void cli_transcript_reset(struct cli_application *ctx);
void cli_transcript_finish(struct cli_application *ctx);

#endif
