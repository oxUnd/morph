#ifndef MORPH_CLI_PRESENTATION_SHORTCUTS_H
#define MORPH_CLI_PRESENTATION_SHORTCUTS_H

#include <nothing/tui/view.h>

/* Standard TextView spans; input strings and styles remain caller-owned. */
int cli_shortcuts_set_spans(struct nt_view *view, const struct nt_text_span *spans,
	size_t count, int styled);
int cli_shortcuts_set_header_spans(struct nt_view *, const struct nt_text_span *,
	size_t count, int styled);

/* Named keys only; preserve per-character styles in status labels. */
int cli_shortcuts_set_named_spans(struct nt_view *, const struct nt_text_span *,
	size_t count, int styled);

#endif
