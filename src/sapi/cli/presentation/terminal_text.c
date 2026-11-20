#include "sapi/cli/presentation/terminal_text.h"

#include <nb/cleanup.h>
#include <nb/utf8.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Reports count code points. Ignore SGR/OSC when measuring a row and remove
 * inline image APCs as well when copying a terminal frame to plain text. */
static size_t cli_terminal_text_escape(const char *text, size_t length, bool strip_images)
{
	if (length < 2 || text[0] != '\033')
		return 0;

	unsigned char kind = (unsigned char)text[1];
	if (kind == '[') {
		size_t i = 2;
		while (i < length && (unsigned char)text[i] < 0x40)
			i++;

		return i < length ? i + 1 : length;
	}

	if (kind == ']' || (strip_images && kind == '_')) {
		for (size_t i = 2; i < length; i++) {
			if (kind == ']' && text[i] == '\a')
				return i + 1;

			if (text[i] == '\033' && i + 1 < length && text[i + 1] == '\\')
				return i + 2;
		}

		return length;
	}

	return 2;
}

size_t cli_terminal_text_width(const char *text)
{
	if (!text)
		return 0;

	const char *end = text + strlen(text);
	size_t columns = 0;
	while (text < end) {
		size_t escaped = cli_terminal_text_escape(text, (size_t)(end - text), false);
		if (escaped) {
			text += escaped;
			continue;
		}

		int32_t cp;
		text = nb_utf8_next_cstr(text, &cp);
		columns += (size_t)nb_utf8_width_codepoint((unsigned)cp);
	}

	return columns;
}

const char *cli_terminal_text_advance(const char *text, size_t columns)
{
	if (!text)
		return NULL;

	while (*text && columns) {
		int32_t cp;
		const char *next = nb_utf8_next_cstr(text, &cp);
		size_t width = (size_t)nb_utf8_width_codepoint((unsigned)cp);
		if (width > columns)
			break;

		columns -= width;
		text = next;
	}

	return text;
}

char *cli_terminal_text_strip_ansi_dup(const char *text, size_t length, size_t *out_length)
{
	if (!text || length == SIZE_MAX)
		return NULL;

	char *plain NB_CLEANUP(nb_charp_cleanup) = malloc(length + 1);
	if (!plain)
		return NULL;

	size_t written = 0;
	for (size_t i = 0; i < length;) {
		size_t escaped = cli_terminal_text_escape(text + i, length - i, true);
		if (escaped)
			i += escaped;
		else
			plain[written++] = text[i++];
	}

	plain[written] = '\0';
	if (out_length)
		*out_length = written;

	return nb_charp_steal(&plain);
}
