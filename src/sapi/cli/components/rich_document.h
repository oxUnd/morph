#ifndef MORPH_CLI_COMPONENTS_RICH_DOCUMENT_H
#define MORPH_CLI_COMPONENTS_RICH_DOCUMENT_H

#include <nothing/tui/view.h>

struct nt_view *cli_rich_document_add_borrow(struct nt_builder *,
	struct nt_view *parent);
/* Trusted renderer output only. Terminal controls become structured content
 * and managed image widgets; they are never forwarded to the terminal. */
int cli_rich_document_set_rendered_copy(struct nt_view *, const char *, int styled);

#endif
