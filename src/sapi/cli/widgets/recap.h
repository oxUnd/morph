#ifndef MORPH_CLI_WIDGETS_RECAP_H
#define MORPH_CLI_WIDGETS_RECAP_H

#include <morph/tui/widget.h>

struct morph_tui_view *cli_recap_add_borrow(struct morph_tui_builder *,
	struct morph_tui_view *parent, const char *summary, const char *next, int styled);

#endif
