#ifndef MORPH_CLI_PAGES_JOBS_H
#define MORPH_CLI_PAGES_JOBS_H

#include "event/event.h"
#include "util/buf.h"
struct cli_application;

int cli_jobs_event(struct cli_application *ctx, const struct morph_event *event);
void cli_jobs_cleanup(struct cli_application *ctx);
int cli_jobs_visible(const struct cli_application *ctx);
int cli_jobs_render(struct cli_application *ctx, morph_buf_t *out, int columns, int styled);
int cli_jobs_waiting(const struct cli_application *ctx);
int cli_jobs_view(struct cli_application *ctx, const char *id);

#endif
