#include <morph/tui/widgets/text_view.h>
#include "util/error.h"
#include "util/utf8.h"

#include <string.h>

int morph_tui_widget_text_layout_wrap_borrow(struct morph_tui_widget_text_layout *view,
					     const char *text, size_t width)
{
	if (!view || !text || !width)
		return -EINVAL;
	if (!view->lines.elts) {
		struct morph_tui_widget_text_line line;
		int rc = morph_array_init(&view->lines, 32, sizeof(line));

		if (rc != 0)
			return rc;
	}
	morph_array_clear(&view->lines);
	view->text = text;
	const char *start = text;

	while (*start) {
		const char *newline = strchr(start, '\n');
		const char *end = utf8_advance_display_width_borrow(start, width);

		if (newline && newline < end)
			end = newline;
		if (end == start && *start != '\n')
			end = utf8_advance_codepoints_borrow(start, 1);
		struct morph_tui_widget_text_line *line = morph_array_push_borrow(&view->lines);

		if (!line)
			return -ENOMEM;
		line->offset = (size_t)(start - text);
		line->length = (size_t)(end - start);
		start = *end == '\n' ? end + 1 : end;
	}
	return 0;
}

void morph_tui_widget_text_layout_cleanup(struct morph_tui_widget_text_layout *view)
{
	if (!view)
		return;
	morph_array_cleanup(&view->lines);
	view->text = NULL;
}
