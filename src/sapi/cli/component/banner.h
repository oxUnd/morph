#ifndef MORPH_CLI_COMPONENT_BANNER_H
#define MORPH_CLI_COMPONENT_BANNER_H

struct cli_application;
struct nt_builder;
struct nt_view;

void cli_banner_print(struct cli_application *ctx, const char *model, const char *directory);
struct nt_view *cli_banner_add_borrow(struct nt_builder *,
	struct nt_view *parent, struct cli_application *ctx);

#endif
