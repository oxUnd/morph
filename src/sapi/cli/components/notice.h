#ifndef MORPH_CLI_COMPONENTS_NOTICE_H
#define MORPH_CLI_COMPONENTS_NOTICE_H

#include <nothing/tui/view.h>

struct cli_notice;

struct nt_view *cli_notice_add_borrow(struct nt_builder *, struct nt_view *parent,
				      const struct cli_notice *, int styled);

#endif
