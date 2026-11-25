#ifndef MORPH_CLI_COMPONENTS_SIDEBAR_NOTICE_H
#define MORPH_CLI_COMPONENTS_SIDEBAR_NOTICE_H

#include <nothing/tui/view.h>

struct nt_view *cli_sidebar_notice_add(struct nt_builder *, struct nt_view *parent,
	const char *text, int styled);
int cli_sidebar_notice_set_text(struct nt_view *, const char *text);

#endif
