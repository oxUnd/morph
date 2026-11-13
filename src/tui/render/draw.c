#include <morph/tui/draw.h>
#include "errors.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>

#include <stdlib.h>
#include <string.h>

int morph_tui_draw_cell_append_aligned(nb_buf *commands, int columns, int rows, int row, int column,
				       int width, const char *text, const char *style, int right)
{
	if (!commands)
		return NB_ERR_INVAL;
	if (nb_buf_status(commands))
		return nb_buf_status(commands);
	if (!text || !style) {
		nb_buf_set_error(commands, NB_ERR_INVAL);
		return NB_ERR_INVAL;
	}
	if (row < 1 || row > rows || column < 1 || column > columns || width <= 0)
		return 0;
	if (width > columns - column + 1)
		width = columns - column + 1;
	char *safe = morph_terminal_sanitize_dup(text, strlen(text),
						 MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);

	if (!safe) {
		nb_buf_set_error(commands, NB_ERR_NOMEM);
		return nb_buf_status(commands);
	}
	/* Ellipsis may add three bytes even when replacing one ASCII byte. */
	size_t capacity = strlen(safe) + 4;
	char *clipped = malloc(capacity);

	if (!clipped) {
		free(safe);
		nb_buf_set_error(commands, NB_ERR_NOMEM);
		return nb_buf_status(commands);
	}
	morph_terminal_copy_ellipsized_display_width(clipped, capacity, safe, (size_t)width, 0);
	int padding = width - (int)nb_utf8_width_cstr(clipped);
	int rc = (nb_buf_printf(commands, "\033[%d;%dH\033[0m%s%*s%s%*s", row, column, style,
				right ? padding : 0, "", clipped, right ? 0 : padding, "")
			  ? 0
			  : NB_ERR_NOMEM);

	free(clipped);
	free(safe);
	return rc;
}

int morph_tui_draw_write(FILE *output, const nb_buf *commands)
{
	if (!output || !commands)
		return NB_ERR_INVAL;
	if (nb_buf_status(commands))
		return nb_buf_status(commands);
	if (!commands->len)
		return 0;
	errno = 0;
	if (fwrite(commands->ptr, 1, commands->len, output) != commands->len)
		return errno ? nb_status_from_errno(errno, NULL) : NB_ERR_IO;
	if (fflush(output) != 0 || ferror(output))
		return errno ? nb_status_from_errno(errno, NULL) : NB_ERR_IO;
	return 0;
}

int morph_tui_draw_cell(FILE *output, int columns, int rows, int row, int column, int width,
			const char *text, const char *style)
{
	nb_buf commands NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&commands), nb_buf_reserve(&commands, 128) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	rc = morph_tui_draw_cell_append(&commands, columns, rows, row, column, width, text, style);
	if (rc == 0)
		rc = morph_tui_draw_write(output, &commands);
	nb_buf_cleanup(&commands);
	return rc;
}

int morph_tui_draw_cell_append(nb_buf *commands, int columns, int rows, int row, int column,
			       int width, const char *text, const char *style)
{
	return morph_tui_draw_cell_append_aligned(commands, columns, rows, row, column, width, text,
						  style, 0);
}
