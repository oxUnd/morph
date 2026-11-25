#ifndef MORPH_CLI_COMPONENTS_DIFF_BROWSER_H
#define MORPH_CLI_COMPONENTS_DIFF_BROWSER_H

#include <nothing/tui/view.h>

struct cli_files_model;
struct nt_view *cli_diff_browser_add(struct nt_builder *, struct nt_view *parent,
				     struct cli_files_model *, const char *workdir, int styled);
int cli_diff_browser_sync(struct nt_view *);
int cli_diff_browser_focus(struct nt_view *);
void cli_diff_browser_reset(struct nt_view *);

#endif
