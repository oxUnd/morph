#ifndef MORPH_CLI_COMPONENTS_BANNER_H
#define MORPH_CLI_COMPONENTS_BANNER_H

struct cli_application;
struct nt_builder;
struct nt_view;

struct nt_view *cli_banner_add_borrow(struct nt_builder *, struct nt_view *parent,
				      struct cli_application *ctx);

#endif
