#ifndef MORPH_CLI_PRESENTATION_MESSAGES_H
#define MORPH_CLI_PRESENTATION_MESSAGES_H

#include <nb/buf.h>
#include <nb/cleanup.h>
#include <nb/vec.h>
#include <nothing/tui/view.h>

enum cli_message_kind {
	CLI_MESSAGE_USER,
	CLI_MESSAGE_REASONING,
	CLI_MESSAGE_ASSISTANT,
};

struct cli_message_style {
	int columns;
	int styled;
	int ultra;
	int trailing_newlines;
	int reasoning_expanded;
	struct nt_view_insets margin;
};

/* Stateless message layout. Only style's local trailing-newline count changes. */
int cli_messages_append(nb_buf *out, enum cli_message_kind kind, const char *text,
			struct cli_message_style *style);
/* Accept only output of trusted renderers whose input was already sanitized.
 * Retain SGR styling, wrap complete UTF-8, and make every physical row standalone. */
int cli_messages_append_rendered(nb_buf *out, const char *bytes, size_t length, int columns,
				 int styled);
/* Bridges trusted Markdown renderer SGR into copied structured text spans. */
int cli_messages_set_rendered_view(struct nt_view *, const char *text, int styled);
int cli_messages_set_rendered_shortcuts_view(struct nt_view *, const char *text, int styled);
int cli_messages_set_rendered_shortcut_header(struct nt_view *, const char *text, int styled);
/* Trusted syntax-highlighter SGR; preserve all source whitespace and line offsets. */
int cli_messages_set_source_view(struct nt_view *, const char *text, int styled);
struct cli_messages_link {
	size_t start, end;
	char *url;
};
void cli_messages_links_cleanup(nb_vec *);
/* Also returns owned link ranges in the displayed UTF-8 byte coordinates. */
int cli_messages_set_rendered_links(struct nt_view *, const char *, int styled, nb_vec *links);
/* Recover link hit ranges from the renderer's visible destinations, using
 * parsed Markdown URLs and labels. Wrapped destinations retain one target. */
int cli_messages_markdown_links(nb_vec *links, const char *display, const char *markdown);

#endif
