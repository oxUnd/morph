#ifndef MORPH_CLI_JOBS_UI_H
#define MORPH_CLI_JOBS_UI_H

#include "event/event.h"
#include "util/buf.h"
struct cli_context;

int cli_jobs_event(struct cli_context *ctx, const struct morph_event *event);
void cli_jobs_cleanup(struct cli_context *ctx);
int cli_jobs_visible(const struct cli_context *ctx);
int cli_jobs_render(struct cli_context *ctx, morph_buf_t *out, int columns, int max_jobs,
		    int styled);
int cli_jobs_view(struct cli_context *ctx, const char *id);
void cli_jobs_acknowledge(struct cli_context *ctx, const char *id);

#endif
