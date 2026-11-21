#ifndef MORPH_CLI_COMPONENTS_SHORTCUT_HINT_H
#define MORPH_CLI_COMPONENTS_SHORTCUT_HINT_H

#include <nothing/tui/view.h>
#include <nothing/tui/text.h>

struct nt_view *cli_shortcut_hint_add(struct nt_builder *, struct nt_view *parent,
	const char *text, const struct nt_view_options *options);
int cli_shortcut_hint_set_text(struct nt_view *, const char *text);
int cli_shortcut_hint_set_wrap_mode(struct nt_view *, enum nt_text_wrap mode);

#endif
