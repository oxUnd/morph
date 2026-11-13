#ifndef MORPH_CLI_WIDGETS_JOBS_H
#define MORPH_CLI_WIDGETS_JOBS_H

#include "sapi/cli/models/jobs.h"
#include "nb/buf.h"
#include "nb/cleanup.h"

int cli_jobs_render(const struct cli_jobs *jobs, nb_buf *out, int columns, int styled);

#endif
