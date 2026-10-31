#include "widgets/counter.h"
#include "util/error.h"

struct sample_customization_widget_counter {
	struct morph_tui_widget base;
	morph_buf_t row;
	int value;
	int step;
};

static int sample_customization_widget_counter_draw(struct morph_tui_view *view,
						    struct morph_tui_canvas *canvas)
{
	struct sample_customization_widget_counter *counter =
		(struct sample_customization_widget_counter *)view;
	unsigned style = morph_tui_view_focused(view) ? MORPH_TUI_STYLE_SELECTED : 0;
	int rc = morph_tui_canvas_fill(canvas, style);

	morph_buf_reset(&counter->row);
	if (!rc)
		rc = morph_buf_printf(&counter->row, "Custom Counter | value %d | step %d",
				      counter->value, counter->step);
	if (!rc)
		rc = morph_tui_canvas_text(canvas, 0, 0, morph_buf_cstr(&counter->row), style);
	morph_buf_reset(&counter->row);
	morph_buf_putc(&counter->row, '[');
	for (int i = 0; i < 20; i++)
		morph_buf_putc(&counter->row, i < counter->value ? '#' : '.');
	morph_buf_putc(&counter->row, ']');
	if (!rc)
		rc = counter->row.failed;
	if (!rc)
		rc = morph_tui_canvas_text(canvas, 0, 1, morph_buf_cstr(&counter->row), style);
	if (!rc)
		rc = morph_tui_canvas_text(canvas, 0, 2,
					   "+/- or Left/Right: value; Space: step; Enter: inspect",
					   style);
	return rc;
}

int sample_customization_widget_counter_set(struct morph_tui_view *view, int value, int step)
{
	if (!view || !view->ops || view->ops->draw != sample_customization_widget_counter_draw)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	if (step != 1 && step != 5)
		return -EINVAL;
	struct sample_customization_widget_counter *counter =
		(struct sample_customization_widget_counter *)view;

	counter->value = value < 0 ? 0 : value > 20 ? 20 : value;
	counter->step = step;
	morph_tui_view_invalidate(view, 0);
	return 0;
}

int sample_customization_widget_counter_value(const struct morph_tui_view *view)
{
	return ((const struct sample_customization_widget_counter *)view)->value;
}

int sample_customization_widget_counter_step(const struct morph_tui_view *view)
{
	return ((const struct sample_customization_widget_counter *)view)->step;
}

static int sample_customization_widget_counter_event(struct morph_tui_view *view,
						     const struct morph_tui_event *event)
{
	if (event->pasting ||
	    (event->type != MORPH_TUI_EVENT_KEY && event->type != MORPH_TUI_EVENT_TEXT))
		return 0;
	struct sample_customization_widget_counter *counter =
		(struct sample_customization_widget_counter *)view;
	int value = counter->value, step = counter->step;
	enum morph_tui_widget_action_type action = MORPH_TUI_WIDGET_ACTION_CHANGED;

	switch (event->key) {
	case '+':
	case '=':
	case MORPH_TUI_KEY_RIGHT:
		value += step;
		break;
	case '-':
	case MORPH_TUI_KEY_LEFT:
		value -= step;
		break;
	case ' ':
		step = step == 1 ? 5 : 1;
		break;
	case '\r':
	case '\n':
		action = MORPH_TUI_WIDGET_ACTION_ACTIVATE;
		break;
	default:
		return 0;
	}
	int rc = sample_customization_widget_counter_set(view, value, step);

	morph_buf_reset(&counter->row);
	if (!rc)
		rc = morph_buf_printf(&counter->row, "Counter action: value %d, step %d",
				      counter->value, counter->step);
	if (!rc)
		rc = morph_tui_widget_emit(view, action, (size_t)counter->value,
					   morph_buf_cstr(&counter->row));
	return rc ? rc : 1;
}

static void sample_customization_widget_counter_destroy(struct morph_tui_view *view)
{
	morph_buf_cleanup(&((struct sample_customization_widget_counter *)view)->row);
}

struct morph_tui_view *sample_customization_widget_counter_create(struct morph_tui_builder *builder,
								  struct morph_tui_view *parent,
								  int tag, int value, int step)
{
	static const struct morph_tui_view_ops methods = {
		.draw = sample_customization_widget_counter_draw,
		.event = sample_customization_widget_counter_event,
		.destroy = sample_customization_widget_counter_destroy,
	};
	const struct morph_tui_view_options options = {.height = 3, .grow = 1};
	struct sample_customization_widget_counter *counter = NULL;

	counter = (struct sample_customization_widget_counter *)morph_tui_widget_create(
		builder, parent, sizeof(*counter), &methods, &options);
	if (!counter)
		return NULL;
	struct morph_tui_view *view = &counter->base.view;

	morph_tui_builder_fail(builder, morph_buf_init(&counter->row, 128));
	morph_tui_builder_fail(builder, sample_customization_widget_counter_set(view, value, step));
	morph_tui_widget_set_tag(view, tag);
	morph_tui_view_set_focusable(view, 1);
	return view;
}
