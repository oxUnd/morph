#ifndef MORPH_CLI_COMPONENTS_SETUP_FORM_H
#define MORPH_CLI_COMPONENTS_SETUP_FORM_H

#include <stdio.h>

#include <nothing/tui/draw.h>
#include <nothing/tui/event.h>

struct cli_setup_state;
struct cli_setup_form;

enum cli_setup_form_result {
	CLI_SETUP_FORM_CONTINUE = 0,
	CLI_SETUP_FORM_SUBMIT = 1,
	CLI_SETUP_FORM_CANCEL = 2
};

int cli_setup_form_create(struct cli_setup_state *state, const char *path, int styled, int columns,
			  struct cli_setup_form **out);
void cli_setup_form_destroy(struct cli_setup_form **form);
int cli_setup_form_show(struct cli_setup_form *form, struct nt_inline_region *region,
			   FILE *output, int columns, int rows);
int cli_setup_form_dispatch(struct cli_setup_form *form, const struct nt_event *event);

#endif
