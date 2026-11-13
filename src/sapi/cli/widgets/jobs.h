#ifndef MORPH_CLI_WIDGETS_JOBS_H
#define MORPH_CLI_WIDGETS_JOBS_H

#include "sapi/cli/models/jobs.h"
#include "util/buf.h"

int cli_jobs_render(const struct cli_jobs *jobs, morph_buf_t *out, int columns, int styled);

#endif
