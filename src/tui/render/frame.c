#include <morph/tui/frame.h>
#include "frame_internal.h"
#include <morph/tui/draw.h>
#include "errors.h"

#include <string.h>

int render_frame_present_composed(struct morph_tui_frame *frame, FILE *output, int columns,
				  int rows, const char *lines, const nb_buf *overlay)
{
	if (!frame || !output || !lines || columns < 1 || rows < 1)
		return NB_ERR_INVAL;
	int resized = frame->rows != rows || frame->columns != columns || frame->target != output;
	size_t size = strlen(lines);

	if (!resized && frame->previous.len == size &&
	    memcmp(nb_buf_cstr(&frame->previous), lines, size) == 0 && (!overlay || !overlay->len))
		return 0;
	if (!frame->previous.ptr) {
		int rc = (nb_buf_init(&frame->previous),
			  nb_buf_reserve(&frame->previous, BUFSIZ) ? 0 : NB_ERR_NOMEM);

		if (rc != 0)
			return rc;
	}
	if (!frame->output.ptr) {
		int rc = (nb_buf_init(&frame->output),
			  nb_buf_reserve(&frame->output, BUFSIZ) ? 0 : NB_ERR_NOMEM);

		if (rc != 0)
			return rc;
	}
	/* Allocate before output; commit the previous frame only after flush. */
	int rc = (nb_buf_reserve(&frame->previous,
				 size > frame->previous.len ? size - frame->previous.len : 0)
			  ? 0
			  : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	const char *current = lines;
	const char *previous = nb_buf_cstr(&frame->previous);
	nb_buf *commands = &frame->output;

	nb_buf_clear(commands);
	nb_buf_append_cstr(commands, "\033[?2026h");
	if (resized)
		nb_buf_append_cstr(commands, "\033[H\033[2J");
	for (int row = 1; row <= rows; row++) {
		size_t length = strcspn(current, "\n");
		size_t old_length = strcspn(previous, "\n");

		if (resized || length != old_length || memcmp(current, previous, length) != 0) {
			nb_buf_printf(commands, "\033[%d;1H", row);
			/* Clear before painting: after a full-width row the terminal cursor
			 * can remain on a wide glyph's trailing cell with wrap pending. */
			nb_buf_append_cstr(commands, "\033[0m\033[2K");
			nb_buf_append(commands, current, length);
			nb_buf_append_cstr(commands, "\033[0m");
		}
		current += length + (current[length] == '\n');
		previous += old_length + (previous[old_length] == '\n');
	}
	if (overlay)
		nb_buf_append(commands, nb_buf_cstr(overlay), overlay->len);
	nb_buf_append_cstr(commands, "\033[?2026l");
	rc = morph_tui_draw_write(output, commands);
	if (rc != 0) {
		morph_tui_frame_invalidate(frame);
		return rc;
	}
	nb_buf_clear(&frame->previous);
	rc = (nb_buf_append(&frame->previous, lines, size) ? 0 : NB_ERR_NOMEM);
	frame->rows = rc == 0 ? rows : 0;
	frame->columns = columns;
	frame->target = output;
	return rc;
}

int morph_tui_frame_present(struct morph_tui_frame *frame, FILE *output, int columns, int rows,
			    const char *lines)
{
	return render_frame_present_composed(frame, output, columns, rows, lines, NULL);
}

void morph_tui_frame_invalidate(struct morph_tui_frame *frame)
{
	if (!frame)
		return;
	frame->columns = 0;
	frame->rows = 0;
	frame->target = NULL;
}

void morph_tui_frame_cleanup(struct morph_tui_frame *frame)
{
	if (!frame)
		return;
	nb_buf_cleanup(&frame->previous);
	nb_buf_cleanup(&frame->output);
	morph_tui_frame_invalidate(frame);
}
