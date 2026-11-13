#include <morph/tui/widgets/card.h>
#include "errors.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>

#include <stdlib.h>
#include <string.h>

int morph_tui_widget_card_border(nb_buf *line, int width, int bottom)
{
	if (!line || width < 2)
		return NB_ERR_INVAL;
	nb_buf_append_cstr(line, bottom ? "└" : "┌");
	for (int i = 0; i < width - 2; i++)
		nb_buf_append_cstr(line, "─");
	nb_buf_append_cstr(line, bottom ? "┘" : "┐");
	return nb_buf_status(line);
}

int morph_tui_widget_card_row(nb_buf *line, int width, const char *text)
{
	if (!line || !text || width < 4)
		return NB_ERR_INVAL;
	char *safe = morph_terminal_sanitize_dup(text, strlen(text),
						 MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);

	if (!safe)
		return NB_ERR_NOMEM;
	char *clipped = malloc(strlen(safe) + 4);

	if (!clipped) {
		free(safe);
		return NB_ERR_NOMEM;
	}
	morph_terminal_copy_ellipsized_display_width(clipped, strlen(safe) + 4, safe,
						     (size_t)(width - 4), 0);
	nb_buf_printf(line, "│ %s", clipped);
	for (size_t i = nb_utf8_width_cstr(clipped); i < (size_t)(width - 4); i++)
		nb_buf_append_char(line, ' ');
	nb_buf_append_cstr(line, " │");
	free(clipped);
	free(safe);
	return nb_buf_status(line);
}
