#ifndef MORPH_CLI_PRESENTATION_TURN_DETAILS_H
#define MORPH_CLI_PRESENTATION_TURN_DETAILS_H

struct cli_application;

int cli_turn_details_is_open(const struct cli_application *ctx);
int cli_turn_details_is_visible(const struct cli_application *ctx);
void cli_turn_details_toggle(struct cli_application *ctx);
void cli_turn_details_cleanup(struct cli_application *ctx);
/* Refresh the retained page, or emit a snapshot for stateless callers. */
void cli_turn_details_render(struct cli_application *ctx, int scroll);

#endif
