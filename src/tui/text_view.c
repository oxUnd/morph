#include <morph/tui/components.h>
#include "util/error.h"
#include "util/utf8.h"

#include <string.h>

int morph_tui_text_view_wrap(struct morph_tui_text_view *view, const char *text, size_t width)
{
	if (!view || !text || !width)
		return -EINVAL;
	if (!view->lines.elts) {
		struct morph_tui_text_line line;
		int rc = morph_array_init(&view->lines, 32, sizeof(line));

		if (rc != 0)
			return rc;
	}
	morph_array_clear(&view->lines);
	view->text = text;
	const char *start = text;

	while (*start) {
		const char *newline = strchr(start, '\n');
		const char *end = utf8_advance_display_width(start, width);

		if (newline && newline < end)
			end = newline;
		if (end == start && *start != '\n')
			end = utf8_advance_codepoints(start, 1);
		struct morph_tui_text_line *line = morph_array_push(&view->lines);

		if (!line)
			return -ENOMEM;
		line->offset = (size_t)(start - text);
		line->length = (size_t)(end - start);
		start = *end == '\n' ? end + 1 : end;
	}
	return 0;
}

void morph_tui_text_view_cleanup(struct morph_tui_text_view *view)
{
	if (!view)
		return;
	morph_array_cleanup(&view->lines);
	view->text = NULL;
}
