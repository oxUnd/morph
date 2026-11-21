#ifndef MORPH_CLI_COMPONENTS_SETUP_FORM_H
#define MORPH_CLI_COMPONENTS_SETUP_FORM_H

#include <nothing/tui/view.h>

struct cli_setup_state;

/* Borrows setup state until the owning page is destroyed. Emits SUBMIT/CANCEL. */
struct nt_view *cli_setup_form_add_borrow(struct nt_builder *, struct nt_view *parent,
					  struct cli_setup_state *, const char *path, int styled);

#endif
