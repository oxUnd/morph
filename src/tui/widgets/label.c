#include <morph/tui/widgets/label.h>
#include "errors.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct widget_label {
	struct morph_tui_widget base;
	char *text;
};

static int widget_label_measure(struct morph_tui_view *view, int width,
				struct morph_tui_view_size *size)
{
	struct widget_label *label = (struct widget_label *)view;
	size_t cells = nb_utf8_width_cstr(label->text ? label->text : "");

	(void)width;
	if (cells > INT_MAX)
		return NB_ERR_RANGE;
	*size = (struct morph_tui_view_size){(int)cells, 1};
	return 0;
}

static int widget_label_draw(struct morph_tui_view *view, struct morph_tui_canvas *canvas)
{
	struct widget_label *label = (struct widget_label *)view;

	return morph_tui_canvas_text(canvas, 0, 0, label->text ? label->text : "", 0);
}

static void widget_label_destroy(struct morph_tui_view *view)
{
	free(((struct widget_label *)view)->text);
}

int morph_tui_widget_label_set_text_copy(struct morph_tui_view *view, const char *text)
{
	if (!view || !text || !view->ops || view->ops->draw != widget_label_draw)
		return NB_ERR_INVAL;
	if (!morph_tui_view_is_owner(view))
		return NB_ERR_NOT_PERMITTED;
	struct widget_label *label = (struct widget_label *)view;

	if (label->text && strcmp(label->text, text) == 0)
		return 0;
	char *copy = morph_terminal_sanitize_dup(text, strlen(text),
						 MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);

	if (!copy)
		return NB_ERR_NOMEM;
	if (label->text && strcmp(label->text, copy) == 0) {
		free(copy);
		return 0;
	}
	int resized = !label->text || nb_utf8_width_cstr(label->text) != nb_utf8_width_cstr(copy);

	free(label->text);
	label->text = copy;
	morph_tui_view_invalidate(view, resized);
	return 0;
}

struct morph_tui_view *
morph_tui_widget_label_add_borrow(struct morph_tui_builder *builder, struct morph_tui_view *parent,
				  const char *text, const struct morph_tui_view_options *options)
{
	static const struct morph_tui_view_ops ops = {
		.measure = widget_label_measure,
		.draw = widget_label_draw,
		.destroy = widget_label_destroy,
	};
	struct widget_label *label = NULL;
	label = (struct widget_label *)morph_tui_widget_add_borrow(builder, parent, sizeof(*label),
								   &ops, options);
	struct morph_tui_view *view = label ? &label->base.view : NULL;

	if (view)
		morph_tui_builder_fail(builder, morph_tui_widget_label_set_text_copy(view, text));
	return view;
}
