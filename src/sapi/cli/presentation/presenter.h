#ifndef MORPH_CLI_PRESENTATION_PRESENTER_H
#define MORPH_CLI_PRESENTATION_PRESENTER_H

#include "event/event.h"

struct cli_application;

void cli_presentation_user_input(struct cli_application *ctx, const char *text);
void cli_presentation_command_input(struct cli_application *ctx, const char *text);
void cli_presentation_command_end(struct cli_application *ctx);
int cli_presentation_init(struct cli_application *ctx);
void cli_presentation_reset(struct cli_application *ctx);
void cli_presentation_finish(struct cli_application *ctx);
void cli_presentation_cleanup(struct cli_application *ctx);
void cli_presentation_prepare_prompt(struct cli_application *ctx);
void cli_presentation_flush_stream(struct cli_application *ctx);
void cli_presentation_patch_diff(struct cli_application *ctx, const char *workdir,
				 const char *input);
int cli_presentation_event(struct cli_application *ctx, const struct morph_event *ev);

#endif
