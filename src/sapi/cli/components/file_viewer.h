#ifndef MORPH_CLI_COMPONENTS_FILE_VIEWER_H
#define MORPH_CLI_COMPONENTS_FILE_VIEWER_H

#include <nothing/tui/view.h>

/* Each viewer retains its native document, search and media viewport. Preview
 * workers own no views and are cancelled before this component is released. */
struct nt_view *cli_file_viewer_add(struct nt_builder *, struct nt_view *parent,
	const char *path, const char *workdir, int styled);
const char *cli_file_viewer_path(const struct nt_view *);
const char *cli_file_viewer_hint(const struct nt_view *);
int cli_file_viewer_open(struct nt_view *, int line, int reload);
int cli_file_viewer_sync(struct nt_view *);
int cli_file_viewer_focus(struct nt_view *);

#endif
