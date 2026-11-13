#include <morph/tui/widgets/text_view.h>
#include "errors.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>

#include <string.h>

int morph_tui_widget_text_layout_wrap_borrow(struct morph_tui_widget_text_layout *view,
					     const char *text, size_t width)
{
	if (!view || !text || !width)
		return NB_ERR_INVAL;
	if (!((void *)view->lines.data)) {
		struct morph_tui_widget_text_line line;
		int rc = (nb_vec_init(&view->lines, sizeof(line)),
			  nb_vec_reserve(&view->lines, 32) ? 0 : NB_ERR_NOMEM);

		if (rc != 0)
			return rc;
	}
	nb_vec_clear(&view->lines);
	view->text = text;
	const char *start = text;
	const char *newline = strchr(start, '\n');

	while (*start) {
		const char *end = morph_terminal_advance_display_width_borrow(start, width);

		if (newline && newline < end)
			end = newline;
		if (end == start && *start != '\n')
			end = nb_utf8_advance(start, 1);
		struct morph_tui_widget_text_line *line = nb_vec_push(&view->lines);

		if (!line)
			return NB_ERR_NOMEM;
		line->offset = (size_t)(start - text);
		line->length = (size_t)(end - start);
		if (*end == '\n') {
			start = end + 1;
			newline = strchr(start, '\n');
		} else
			start = end;
	}
	return 0;
}

void morph_tui_widget_text_layout_cleanup(struct morph_tui_widget_text_layout *view)
{
	if (!view)
		return;
	nb_vec_cleanup(&view->lines);
	view->text = NULL;
}
