#ifndef MORPH_CLI_PRESENTATION_HISTORY_H
#define MORPH_CLI_PRESENTATION_HISTORY_H

struct cli_application;
struct model_history_item;
struct message;

int cli_history_render_session(struct cli_application *ctx, struct model_history_item *items,
			       const struct message *messages);

#endif
