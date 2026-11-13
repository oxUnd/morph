#ifndef MORPH_CLI_WIDGETS_RICH_DOCUMENT_H
#define MORPH_CLI_WIDGETS_RICH_DOCUMENT_H

#include <morph/tui/widget.h>

struct morph_tui_view *cli_rich_document_add_borrow(struct morph_tui_builder *,
	struct morph_tui_view *parent);
/* Trusted renderer output only. Terminal controls become structured content
 * and managed image widgets; they are never forwarded to the terminal. */
int cli_rich_document_set_rendered_copy(struct morph_tui_view *, const char *, int styled);

#endif
