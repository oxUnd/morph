#include "sapi/cli/widgets/recap.h"
#include "sapi/cli/presentation/recap.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/style.h"
#include "nb/cleanup.h"
#include "nb/err.h"
#include <morph/tui/widgets/text_view.h>
#include <stdlib.h>
#include <string.h>

struct cli_recap_widget {
	struct morph_tui_widget base;
	struct morph_tui_view *body;
	char *summary, *next;
	int columns, styled;
};

static int cli_recap_widget_measure(struct morph_tui_view *view, int width,
				   struct morph_tui_view_size *size)
{
	struct cli_recap_widget *widget = (struct cli_recap_widget *)view;
	if (widget->columns != width) {
		nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
		int rc = cli_recap_append(&text, widget->summary, widget->next,
			width + (int)CLI_CONTENT_RIGHT_PADDING, widget->styled);
		while (!rc && text.len && text.ptr[text.len - 1] == '\n')
			text.ptr[--text.len] = '\0';
		if (!rc)
			rc = cli_messages_set_rendered_view(widget->body, nb_buf_cstr(&text),
				widget->styled);
		if (rc)
			return rc;
		widget->columns = width;
	}
	return morph_tui_view_measure(widget->body, width, size);
}

static int cli_recap_widget_layout(struct morph_tui_view *view)
{
	return morph_tui_view_arrange(((struct cli_recap_widget *)view)->body,
		morph_tui_view_content_rect(view));
}

static void cli_recap_widget_destroy(struct morph_tui_view *view)
{
	struct cli_recap_widget *widget = (struct cli_recap_widget *)view;
	free(widget->summary);
	free(widget->next);
}

static const struct morph_tui_view_ops cli_recap_widget_ops = {
	.measure = cli_recap_widget_measure,
	.layout = cli_recap_widget_layout,
	.destroy = cli_recap_widget_destroy,
};

struct morph_tui_view *cli_recap_add_borrow(struct morph_tui_builder *builder,
	struct morph_tui_view *parent, const char *summary, const char *next, int styled)
{
	if (!summary) {
		morph_tui_builder_fail(builder, NB_ERR_INVAL);
		return NULL;
	}
	const struct morph_tui_view_options options = {
		.margin = {.top = 1, .bottom = 1},
		.padding = {.right = (int)CLI_CONTENT_RIGHT_PADDING}};
	struct cli_recap_widget *widget = (struct cli_recap_widget *)morph_tui_widget_add_borrow(
		builder, parent, sizeof(*widget), &cli_recap_widget_ops, &options);
	if (!widget)
		return NULL;
	widget->summary = strdup(summary);
	widget->next = strdup(next ? next : "");
	if (!widget->summary || !widget->next)
		morph_tui_builder_fail(builder, NB_ERR_NOMEM);
	widget->columns = -1;
	widget->styled = styled;
	widget->body = morph_tui_widget_text_view_add_borrow(builder, &widget->base.view, "", NULL);
	if (widget->body) {
		morph_tui_builder_fail(builder,
			morph_tui_widget_text_view_set_content_sizing(widget->body, 1));
		morph_tui_view_set_focusable(widget->body, 0);
	}
	return &widget->base.view;
}
