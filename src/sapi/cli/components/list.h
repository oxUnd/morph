#ifndef MORPH_CLI_COMPONENTS_LIST_H
#define MORPH_CLI_COMPONENTS_LIST_H

#include <nothing/tui/view.h>

struct nt_view *cli_list_add(struct nt_builder *builder, struct nt_view *parent,
			     const struct nt_view_options *options);

#endif
