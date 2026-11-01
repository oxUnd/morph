#ifndef MORPH_CLI_UI_PRESENTATION_PRESENTER_H
#define MORPH_CLI_UI_PRESENTATION_PRESENTER_H

#include "event/event.h"

struct cli_context;

void cli_presentation_user_input(struct cli_context *ctx, const char *text);
void cli_presentation_command_input(struct cli_context *ctx, const char *text);
void cli_presentation_command_end(struct cli_context *ctx);
int cli_presentation_init(struct cli_context *ctx);
void cli_presentation_reset(struct cli_context *ctx);
void cli_presentation_finish(struct cli_context *ctx);
void cli_presentation_cleanup(struct cli_context *ctx);
void cli_presentation_prepare_prompt(struct cli_context *ctx);
void cli_presentation_flush_stream(struct cli_context *ctx);
void cli_presentation_patch_diff(struct cli_context *ctx, const char *workdir, const char *input);
int cli_presentation_event(struct cli_context *ctx, const struct morph_event *ev);

#endif
