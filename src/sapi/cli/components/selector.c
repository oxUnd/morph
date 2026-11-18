#include "sapi/cli/components/selector.h"
#include "sapi/cli/presentation/panel_style.h"

#include "sapi/cli/application.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include <nb/err.h>
#include <nb/buf.h>
#include <nb/cleanup.h>
#include <limits.h>
#include <nothing/tui/terminal/text.h>
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/views/label.h>
#include <stdlib.h>
#include <string.h>

struct cli_selector {
	struct cli_application *ctx;
	const struct cli_selector_choice *choices;
	const char *title;
	char *subtitle, *detail;
	size_t count, submitted;
	int current;
	void (*apply)(struct cli_application *, size_t);
};

struct cli_selector_view {
	struct nt_view base;
	struct cli_selector *selector;
	struct nt_view *heading, *subtitle_view, *detail_view, *list, *description, *hint;
	struct nt_text_style normal;
};

struct cli_selector_rows {
	int subtitle, detail, options, description, footer;
};

static struct cli_selector_rows cli_selector_rows(const struct cli_selector *selector)
{
	struct nt_terminal_environment env = {0};
	nt_application_environment(&selector->ctx->base, &env);
	int budget = env.rows > 4 ? env.rows - 4 : 1;
	struct cli_selector_rows rows = {
		.subtitle = budget >= 7,
		.detail = selector->detail && budget >= 8,
		.description = budget >= 10,
		.footer = budget >= 4,
	};
	rows.options =
		budget - 1 - rows.subtitle - rows.detail - rows.description * 2 - rows.footer;
	if (rows.options < 0)
		rows.options = 0;
	if ((size_t)rows.options > selector->count)
		rows.options = (int)selector->count;
	return rows;
}

static int cli_selector_measure(struct nt_view *view, int width, struct nt_view_size *size)
{
	struct cli_selector_rows rows =
		cli_selector_rows(((struct cli_selector_view *)view)->selector);
	*size = (struct nt_view_size){width, 1 + rows.subtitle + rows.detail + rows.options +
						     rows.description * 2 + rows.footer};
	return 0;
}

static int cli_selector_place(struct nt_view *view, struct nt_view_rect *slot, int rows)
{
	slot->height = rows;
	int rc = nt_view_arrange(view, *slot);
	slot->y += rows;
	return rc;
}

static int cli_selector_layout(struct nt_view *view)
{
	struct cli_selector_view *s = (struct cli_selector_view *)view;
	struct cli_selector_rows rows = cli_selector_rows(s->selector);
	struct nt_view_rect slot = nt_view_content_rect(view);
	int rc = cli_selector_place(s->heading, &slot, 1);
	if (!rc)
		rc = cli_selector_place(s->subtitle_view, &slot, rows.subtitle);
	if (!rc)
		rc = cli_selector_place(s->detail_view, &slot, rows.detail);
	if (!rc)
		rc = cli_selector_place(s->list, &slot, rows.options);
	slot.y += rows.description;
	if (!rc)
		rc = cli_selector_place(s->description, &slot, rows.description);
	if (!rc)
		rc = cli_selector_place(s->hint, &slot, rows.footer);
	return rc;
}

static int cli_selector_draw(struct nt_view *view, struct nt_canvas *canvas)
{
	struct cli_selector_view *selector = (struct cli_selector_view *)view;
	nt_canvas_cursor(canvas, 0, 0);
	return nt_canvas_fill_styled(canvas, &selector->normal);
}

static int cli_selector_row(void *data, struct nt_canvas *canvas, size_t item, size_t group,
			    int selected)
{
	struct cli_selector_view *view = data;
	struct cli_selector *s = view->selector;
	(void)group;
	struct nt_text_style style = view->normal;
	if (selected && cli_output_color_enabled(s->ctx)) {
		style.foreground = (struct nt_color){NT_COLOR_INDEXED, 15};
		style.background = (struct nt_color){NT_COLOR_INDEXED, 4};
	}
	const char *label = s->choices[item].label;
	char *safe NB_CLEANUP(nb_charp_cleanup) =
		nt_sanitize_dup(label, strlen(label), NT_TEXT_SINGLE_LINE, NULL);
	nb_buf row NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!safe || !nb_buf_printf(&row, "%c %zu. %s%s", selected ? '>' : ' ', item + 1, safe,
				    s->current == (int)item ? " (current)" : ""))
		return NB_ERR_NOMEM;
	int rc = nt_canvas_fill_styled(canvas, &style);
	return rc ? rc : nt_canvas_text_styled(canvas, 0, 0, nb_buf_cstr(&row), &style);
}

static int cli_selector_description(void *data, struct nt_value *value)
{
	struct cli_selector_view *view = data;
	size_t row = nt_list_view_selected(view->list);
	return nt_value_set_string(value, view->selector->choices[row].description);
}

static int cli_selector_activate(void *data, const struct nt_view_event *event)
{
	struct cli_selector *s = data;
	s->submitted = event->index;
	return cli_chat_panel_close(s->ctx, 0);
}

static struct nt_view *cli_selector_build(struct nt_builder *builder, struct nt_view *parent,
					  void *data)
{
	static const struct nt_view_class type = {.superclass = &nt_view_base_class,
						  .instance_size = sizeof(struct cli_selector_view),
						  .measure = cli_selector_measure,
						  .layout = cli_selector_layout,
						  .draw = cli_selector_draw};
	struct cli_selector *s = data;
	struct cli_selector_view *layout = (struct cli_selector_view *)nt_view_add(
		builder, parent, &type, &(struct nt_view_options){.margin = {.right = 1}});
	if (!layout)
		return NULL;
	layout->selector = s;
	struct nt_view *root = &layout->base;
	struct nt_view_options title = {0}, dim = {0};
	if (cli_output_color_enabled(s->ctx)) {
		layout->normal.background =
			(struct nt_color){NT_COLOR_INDEXED, CLI_INPUT_BACKGROUND_INDEX};
		title = cli_panel_heading(NT_AUTO);
		dim = cli_panel_hint(NT_AUTO);
		title.style.background = dim.style.background = layout->normal.background;
	}
	size_t initial = s->current >= 0 ? (size_t)s->current : 0;
	layout->heading = nt_label_add(builder, root, s->title, &title);
	layout->subtitle_view = nt_label_add(builder, root, s->subtitle, &dim);
	layout->detail_view = nt_label_add(builder, root, s->detail ? s->detail : "", &dim);
	layout->list = nt_list_view_add(builder, root, NULL, 0, 0, NULL);
	layout->description = nt_label_add(builder, root, "", &dim);
	layout->hint =
		nt_label_add(builder, root, "  Up/Down Select  Enter Apply  Esc Cancel", &dim);
	if (nt_builder_error(builder))
		return NULL;
	struct nt_view *labels[] = {layout->heading, layout->subtitle_view, layout->detail_view,
				    layout->description, layout->hint};
	for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++)
		nt_builder_fail(builder, nt_label_set_opaque(labels[i], 1));
	int rc = nt_list_view_set_source(layout->list, s->count, NULL, NULL);
	if (!rc)
		rc = nt_list_view_set_row_renderer(layout->list, cli_selector_row, layout);
	if (!rc)
		rc = nt_list_view_set_navigation(layout->list,
						 NT_LIST_VIEW_WRAP | NT_LIST_VIEW_DIGITS);
	if (!rc)
		rc = nt_list_view_select(layout->list, initial);
	if (!rc)
		rc = nt_view_bind_compute(layout->description, NT_PROP_TEXT,
					  cli_selector_description, layout);
	if (!rc && !nt_view_on(layout->list, NT_VIEW_EVENT_ACTIVATE, cli_selector_activate, s))
		rc = NB_ERR_NOMEM;
	nt_builder_fail(builder, rc);
	return layout->list;
}

static void cli_selector_complete(void *data, int result)
{
	struct cli_selector *s = data;
	if (!result && s->submitted < s->count)
		s->apply(s->ctx, s->submitted);
	free(s->detail);
	free(s->subtitle);
	free(s);
}

int cli_selector_show(struct cli_application *ctx, const char *title, const char *subtitle,
		      const char *detail, const struct cli_selector_choice *choices, size_t count,
		      int current, void (*apply)(struct cli_application *, size_t))
{
	if (!ctx || !title || !choices || !count || count > INT_MAX || !apply ||
	    current >= (int)count)
		return NB_ERR_INVAL;
	for (size_t i = 0; i < count; i++)
		if (!choices[i].label || !choices[i].description)
			return NB_ERR_INVAL;
	struct cli_selector *s = calloc(1, sizeof(*s));
	if (!s)
		return NB_ERR_NOMEM;
	*s = (struct cli_selector){.ctx = ctx,
				   .title = title,
				   .choices = choices,
				   .count = count,
				   .current = current,
				   .submitted = SIZE_MAX,
				   .apply = apply};
	s->subtitle = strdup(subtitle ? subtitle : "");
	s->detail = detail ? strdup(detail) : NULL;
	int rc = s->subtitle && (!detail || s->detail)
			 ? cli_chat_panel_mount(ctx, cli_selector_build, s, cli_selector_complete)
			 : NB_ERR_NOMEM;
	if (rc)
		cli_selector_complete(s, rc);
	return rc;
}
