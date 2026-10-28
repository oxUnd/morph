#include <morph/tui.h>
#include "util/error.h"
#include "util/utf8.h"

#include <stdlib.h>
#include <string.h>

int morph_tui_cell_append_aligned(morph_buf_t *commands, int columns, int rows,
			    int row, int column, int width, const char *text,
			    const char *style, int right)
{
	if (!commands)
		MORPH_RETURN(-EINVAL);
	if (commands->failed)
		MORPH_RETURN(commands->failed);
	if (!text || !style) {
		MORPH_SET_ERR(commands->failed, -EINVAL);
		MORPH_RETURN(commands->failed);
	}
	if (row < 1 || row > rows || column < 1 || column > columns || width <= 0)
		return 0;
	if (width > columns - column + 1)
		width = columns - column + 1;
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);

	if (!safe) {
		MORPH_SET_ERR(commands->failed, -ENOMEM);
		MORPH_RETURN(commands->failed);
	}
	/* Ellipsis may add three bytes even when replacing one ASCII byte. */
	size_t capacity = strlen(safe) + 4;
	char *clipped = malloc(capacity);

	if (!clipped) {
		free(safe);
		MORPH_SET_ERR(commands->failed, -ENOMEM);
		MORPH_RETURN(commands->failed);
	}
	utf8_copy_ellipsized_display_width(clipped, capacity, safe, (size_t)width, 0);
	int padding = width - (int)utf8_display_width(clipped);
	int rc = morph_buf_printf(commands, "\033[%d;%dH%s%*s%s%*s",
		row, column, style, right ? padding : 0, "", clipped,
		right ? 0 : padding, "");

	free(clipped);
	free(safe);
	MORPH_RETURN(rc);
}

int morph_tui_output_write(FILE *output, const morph_buf_t *commands)
{
	if (!output || !commands)
		MORPH_RETURN(-EINVAL);
	if (commands->failed)
		MORPH_RETURN(commands->failed);
	if (!commands->len)
		return 0;
	errno = 0;
	if (fwrite(commands->data, 1, commands->len, output) != commands->len)
		MORPH_RETURN(errno ? -errno : -EIO);
	if (fflush(output) != 0 || ferror(output))
		MORPH_RETURN(errno ? -errno : -EIO);
	return 0;
}

int morph_tui_cell(FILE *output, int columns, int rows, int row, int column,
	     int width, const char *text, const char *style)
{
	morph_buf_t commands = {0};
	int rc = morph_buf_init(&commands, 128);

	if (rc != 0)
		MORPH_RETURN(rc);
	rc = morph_tui_cell_append(&commands, columns, rows, row, column, width, text, style);
	if (rc == 0)
		rc = morph_tui_output_write(output, &commands);
	morph_buf_cleanup(&commands);
	MORPH_RETURN(rc);
}

int morph_tui_cell_append(morph_buf_t *commands, int columns, int rows,
		    int row, int column, int width, const char *text,
		    const char *style)
{
	return morph_tui_cell_append_aligned(commands, columns, rows,
		row, column, width, text, style, 0);
}
