#ifndef MORPH_CLI_PRESENTATION_JOBS_H
#define MORPH_CLI_PRESENTATION_JOBS_H

#include "event/event.h"

struct cli_application;

int cli_jobs_event(struct cli_application *ctx, const struct morph_event *event);

#endif
