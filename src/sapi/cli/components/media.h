#ifndef MORPH_CLI_COMPONENTS_MEDIA_H
#define MORPH_CLI_COMPONENTS_MEDIA_H

#include <nothing/tui/view.h>

struct cli_application;

struct nt_view *cli_media_add_borrow(struct nt_builder *, struct nt_view *parent,
				     struct cli_application *, const char *type, const char *path,
				     const struct nt_view_options *);

#endif
