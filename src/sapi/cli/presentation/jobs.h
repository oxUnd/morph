#ifndef MORPH_CLI_PRESENTATION_JOBS_H
#define MORPH_CLI_PRESENTATION_JOBS_H

#include "event/event.h"
#include "sapi/cli/models/jobs.h"
#include <nb/buf.h>
#include <nb/cleanup.h>

struct cli_application;

int cli_jobs_event(struct cli_application *ctx, const struct morph_event *event);

int cli_jobs_render(const struct cli_jobs *jobs, nb_buf *out, int columns, int styled, int64_t now);
int cli_jobs_activity_render(const struct cli_jobs *, nb_buf *summary, nb_buf *details,
	int columns, int styled);

#endif
