#ifndef MORPH_CLI_PAGES_SUBAGENTS_H
#define MORPH_CLI_PAGES_SUBAGENTS_H

struct cli_application;
struct morph_event;

int cli_subagents_show(struct cli_application *ctx, const char *id);
int cli_subagents_show_fullscreen(struct cli_application *ctx, const char *id);
int cli_subagents_event(struct cli_application *ctx, const struct morph_event *event);
void cli_subagents_cleanup(struct cli_application *ctx);

#endif
