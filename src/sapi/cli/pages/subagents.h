#ifndef MORPH_CLI_PAGES_SUBAGENTS_H
#define MORPH_CLI_PAGES_SUBAGENTS_H

#include "event/event.h"
struct cli_application;

int cli_subagents_show(struct cli_application *ctx, const char *id);
int cli_subagents_event(struct cli_application *ctx, const struct morph_event *event);
int cli_subagents_is_open(const struct cli_application *ctx);
void cli_subagents_cleanup(struct cli_application *ctx);

#endif
