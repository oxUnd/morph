#ifndef MORPH_CLI_PAGES_TURN_DETAILS_H
#define MORPH_CLI_PAGES_TURN_DETAILS_H

struct cli_application;

int cli_turn_details_is_open(const struct cli_application *ctx);
int cli_turn_details_is_visible(const struct cli_application *ctx);
void cli_turn_details_toggle(struct cli_application *ctx);
void cli_turn_details_cleanup(struct cli_application *ctx);
/* Render a page snapshot; terminal lifecycle remains with Application. */
void cli_turn_details_render(struct cli_application *ctx, int scroll);

#endif
