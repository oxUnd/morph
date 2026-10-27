#include "tui/tui.h"
#include "util/error.h"

#include <string.h>

int tui_frame_present(struct tui_frame *frame, FILE *output,
		      int columns, int rows, const char *lines)
{
	if (!frame || !output || !lines || columns < 1 || rows < 1)
		MORPH_RETURN(-EINVAL);
	int resized = frame->rows != rows || frame->columns != columns ||
		frame->target != output;
	size_t size = strlen(lines);

	if (!resized && frame->previous.len == size &&
	    memcmp(morph_buf_cstr(&frame->previous), lines, size) == 0)
		return 0;
	if (!frame->previous.data) {
		int rc = morph_buf_init(&frame->previous, BUFSIZ);

		if (rc != 0)
			MORPH_RETURN(rc);
	}
	if (!frame->output.data) {
		int rc = morph_buf_init(&frame->output, BUFSIZ);

		if (rc != 0)
			MORPH_RETURN(rc);
	}
	/* Allocate before output; commit the previous frame only after flush. */
	int rc = morph_buf_reserve(&frame->previous,
		size > frame->previous.len ? size - frame->previous.len : 0);

	if (rc != 0)
		MORPH_RETURN(rc);
	const char *current = lines;
	const char *previous = morph_buf_cstr(&frame->previous);
	morph_buf_t *commands = &frame->output;

	morph_buf_reset(commands);
	morph_buf_puts(commands, "\033[?2026h");
	if (resized)
		morph_buf_puts(commands, "\033[H\033[2J");
	for (int row = 1; row <= rows; row++) {
		size_t length = strcspn(current, "\n");
		size_t old_length = strcspn(previous, "\n");

		if (resized || length != old_length ||
		    memcmp(current, previous, length) != 0) {
			morph_buf_printf(commands, "\033[%d;1H", row);
			morph_buf_append(commands, current, length);
			morph_buf_puts(commands, "\033[0m\033[K");
		}
		current += length + (current[length] == '\n');
		previous += old_length + (previous[old_length] == '\n');
	}
	morph_buf_puts(commands, "\033[?2026l");
	rc = tui_output_write(output, commands);
	if (rc != 0) {
		tui_frame_invalidate(frame);
		MORPH_RETURN(rc);
	}
	morph_buf_reset(&frame->previous);
	rc = morph_buf_append(&frame->previous, lines, size);
	frame->rows = rc == 0 ? rows : 0;
	frame->columns = columns;
	frame->target = output;
	MORPH_RETURN(rc);
}

void tui_frame_invalidate(struct tui_frame *frame)
{
	if (!frame)
		return;
	frame->columns = 0;
	frame->rows = 0;
	frame->target = NULL;
}

void tui_frame_cleanup(struct tui_frame *frame)
{
	if (!frame)
		return;
	morph_buf_cleanup(&frame->previous);
	morph_buf_cleanup(&frame->output);
	tui_frame_invalidate(frame);
}
