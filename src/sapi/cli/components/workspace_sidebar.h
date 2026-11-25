#ifndef MORPH_CLI_COMPONENTS_WORKSPACE_SIDEBAR_H
#define MORPH_CLI_COMPONENTS_WORKSPACE_SIDEBAR_H

#include <nothing/tui/view.h>

struct cli_files_model;
struct nt_view *cli_workspace_sidebar_add(struct nt_builder *, struct nt_view *parent,
					  struct cli_files_model *, const char *workdir,
					  int styled);
int cli_workspace_sidebar_sync(struct nt_view *);
int cli_workspace_sidebar_focus(struct nt_view *);
int cli_workspace_sidebar_focused(const struct nt_view *);
int cli_workspace_sidebar_open(struct nt_view *, const char *path, int line);
int cli_workspace_sidebar_list(struct nt_view *);
void cli_workspace_sidebar_reset(struct nt_view *);

#endif
