#ifndef MORPH_CLI_COMPONENTS_RECAP_H
#define MORPH_CLI_COMPONENTS_RECAP_H

#include <nothing/tui/view.h>

struct nt_view *cli_recap_add_borrow(struct nt_builder *, struct nt_view *parent,
				     const char *summary, const char *next, int styled);

#endif
