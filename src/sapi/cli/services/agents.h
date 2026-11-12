#ifndef MORPH_CLI_SERVICES_AGENTS_H
#define MORPH_CLI_SERVICES_AGENTS_H
#include "event/event.h"
#include "util/buf.h"
struct cli_application;
int cli_agents_receipts_init(struct cli_application *ctx);
void cli_agents_receipts_cleanup(struct cli_application *ctx);
void cli_agents_flush_receipts(struct cli_application *ctx);
int cli_agents_event(struct cli_application *ctx, const struct morph_event *event);
int cli_agents_visible(const struct cli_application *ctx);
int cli_agents_render(struct cli_application *ctx, morph_buf_t *out, morph_buf_t *summary,
		      int columns, int maximum, int styled);
#endif
