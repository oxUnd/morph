#ifndef MORPH_CLI_COMPONENT_SETUP_PROMPT_H
#define MORPH_CLI_COMPONENT_SETUP_PROMPT_H

#include <nothing/tui/draw.h>
#include <nothing/tui/models/list_navigation.h>
#include <nothing/tui/models/text_editor.h>

struct cli_setup_prompt;

/* The prompt borrows its immutable labels and its mutable navigation/editor
 * state for the lifetime of this question. */
struct cli_setup_prompt_options {
	const char *title, *detail, *progress, *scope, *summary;
	const char *const *items, *const *descriptions;
	struct nt_menu_state *menu;
	struct nt_text_editor_state *editor;
	int count, roles, current, confirmed, styled;
};

int cli_setup_prompt_create(const struct cli_setup_prompt_options *, struct cli_setup_prompt **out);
int cli_setup_prompt_present(struct cli_setup_prompt *, struct nt_inline_region *, FILE *,
			     int columns, int rows, const char *validation);
void cli_setup_prompt_cleanup(struct cli_setup_prompt **);

#endif
