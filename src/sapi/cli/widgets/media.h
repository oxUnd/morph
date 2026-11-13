#ifndef MORPH_CLI_WIDGETS_MEDIA_H
#define MORPH_CLI_WIDGETS_MEDIA_H

#include <morph/tui/widget.h>

struct cli_application;

struct morph_tui_view *cli_media_add_borrow(struct morph_tui_builder *,
	struct morph_tui_view *parent, struct cli_application *, const char *type,
	const char *path, const struct morph_tui_view_options *);

#endif
