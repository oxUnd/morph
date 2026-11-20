#ifndef MORPH_CLI_COMPONENTS_FILE_BROWSER_H
#define MORPH_CLI_COMPONENTS_FILE_BROWSER_H

#include <nothing/tui/view.h>

struct cli_files_model;

/* Borrows the conversation inventory. OPEN carries the selected absolute path;
 * CANCEL closes the sidebar. Each file tab retains its own native viewer. */
struct nt_view *cli_file_browser_add(struct nt_builder *, struct nt_view *parent,
	struct cli_files_model *, const char *workdir, int styled);
int cli_file_browser_sync(struct nt_view *);
int cli_file_browser_focus(struct nt_view *);
int cli_file_browser_list(struct nt_view *);
int cli_file_browser_focused(const struct nt_view *);
int cli_file_browser_open(struct nt_view *, const char *path, int line);
void cli_file_browser_reset(struct nt_view *);

#endif
