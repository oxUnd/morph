#ifndef MORPH_CLI_WIDGETS_BANNER_H
#define MORPH_CLI_WIDGETS_BANNER_H

struct cli_application;
struct morph_tui_builder;
struct morph_tui_view;

void cli_banner_print(struct cli_application *ctx, const char *model, const char *directory);
struct morph_tui_view *cli_banner_add_borrow(struct morph_tui_builder *,
	struct morph_tui_view *parent, struct cli_application *ctx);

#endif
