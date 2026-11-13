#ifndef MORPH_CLI_PRESENTATION_AGENTS_H
#define MORPH_CLI_PRESENTATION_AGENTS_H

#include "event/event.h"

struct cli_application;

int cli_presentation_agents_event(struct cli_application *ctx,
				  const struct morph_event *event);
void cli_presentation_agents_flush(struct cli_application *ctx);

#endif
