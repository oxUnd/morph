#include "tui/components.h"
#include "util/error.h"
#include "util/utf8.h"

#include <stdlib.h>
#include <string.h>

int tui_card_border(morph_buf_t *line, int width, int bottom)
{
	if (!line || width < 2)
		MORPH_RETURN(-EINVAL);
	morph_buf_puts(line, bottom ? "└" : "┌");
	for (int i = 0; i < width - 2; i++)
		morph_buf_puts(line, "─");
	morph_buf_puts(line, bottom ? "┘" : "┐");
	MORPH_RETURN(line->failed);
}

int tui_card_row(morph_buf_t *line, int width, const char *text)
{
	if (!line || !text || width < 4)
		MORPH_RETURN(-EINVAL);
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);

	if (!safe)
		MORPH_RETURN(-ENOMEM);
	char *clipped = malloc(strlen(safe) + 4);

	if (!clipped) {
		free(safe);
		MORPH_RETURN(-ENOMEM);
	}
	utf8_copy_ellipsized_display_width(clipped, strlen(safe) + 4, safe,
		(size_t)(width - 4), 0);
	morph_buf_printf(line, "│ %s", clipped);
	for (size_t i = utf8_display_width(clipped); i < (size_t)(width - 4); i++)
		morph_buf_putc(line, ' ');
	morph_buf_puts(line, " │");
	free(clipped);
	free(safe);
	MORPH_RETURN(line->failed);
}
