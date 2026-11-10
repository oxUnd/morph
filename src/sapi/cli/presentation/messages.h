#ifndef MORPH_CLI_PRESENTATION_MESSAGES_H
#define MORPH_CLI_PRESENTATION_MESSAGES_H

#include "util/buf.h"

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
};

/* Stateless message layout. Only style's local trailing-newline count changes. */
int cli_messages_append(morph_buf_t *out, enum cli_message_kind kind, const char *text,
			struct cli_message_style *style);
/* Accept only output of trusted renderers whose input was already sanitized.
 * Retain SGR styling, wrap complete UTF-8, and make every physical row standalone. */
int cli_messages_append_rendered(morph_buf_t *out, const char *bytes, size_t length, int columns,
				 int styled);

#endif
