#ifndef MORPH_CLI_COMPONENTS_STATUS_BAR_H
#define MORPH_CLI_COMPONENTS_STATUS_BAR_H

#include <nothing/tui/view.h>

struct nt_view *cli_status_bar_add(struct nt_builder *, struct nt_view *parent, int styled);
int cli_status_bar_set_text(struct nt_view *, const char *text);

#endif
