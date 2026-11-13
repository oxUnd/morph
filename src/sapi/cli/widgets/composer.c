#include "sapi/cli/widgets/composer.h"
#include "sapi/cli/application.h"
#include "sapi/cli/models/composer.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include <morph/tui/widgets/stack.h>
#include <morph/tui/widgets/text_field.h>
#include <errno.h>
#include <limits.h>

struct cli_composer {
	struct morph_tui_widget base;
	struct morph_tui_view *stack;
	struct cli_application *ctx;
	int frame;
};

static int cli_composer_decorate(void *data, const char *text, size_t offset,
	struct morph_tui_text_style *style)
{
	struct cli_application *ctx = data;
	if (!cli_output_color_enabled(ctx) || offset > INT_MAX)
		return 0;
	int start, end;
	if (cli_composer_model_image_span(&ctx->composer, text, (int)offset, 0, &start, &end)) {
		style->foreground = (struct morph_tui_color){MORPH_TUI_COLOR_INDEXED, 153};
		style->background = (struct morph_tui_color){MORPH_TUI_COLOR_INDEXED, 24};
	}
	return 0;
}

static int cli_composer_atomic_range(void *data, const char *text, size_t cursor, int backward,
	size_t *start, size_t *end)
{
	struct cli_application *ctx = data;
	if (cursor > INT_MAX)
		return 0;
	int first, last;
	if (!cli_composer_model_image_span(&ctx->composer, text, (int)cursor, backward,
		&first, &last))
		return 0;
	*start = (size_t)first;
	*end = (size_t)last;
	return 1;
}

static int cli_composer_measure(struct morph_tui_view *view, int width,
				struct morph_tui_view_size *size)
{
	return morph_tui_view_measure(((struct cli_composer *)view)->stack, width, size);
}

static int cli_composer_layout(struct morph_tui_view *view)
{
	return morph_tui_view_arrange(((struct cli_composer *)view)->stack,
				      morph_tui_view_content_rect(view));
}

static int cli_composer_draw_padding(struct morph_tui_view *view, enum morph_tui_view_edge edge,
				     struct morph_tui_canvas *canvas)
{
	struct cli_composer *composer = (struct cli_composer *)view;
	if (composer->frame < 0 || composer->frame >= 30 ||
	    (edge != MORPH_TUI_VIEW_EDGE_TOP && edge != MORPH_TUI_VIEW_EDGE_BOTTOM))
		return 0;
	int center = (int)((int64_t)composer->frame * canvas->width / 30);
	for (int cell = 0; cell < canvas->width; cell++) {
		int distance = cell > center ? cell - center : center - cell;
		if (distance >= 8)
			continue;
		unsigned color = distance < 2 ? 141u : distance < 4 ? 98u
					       : distance < 6 ? 54u : 17u;
		struct morph_tui_text_style style = {
			.background = {MORPH_TUI_COLOR_INDEXED, color}};
		for (int row = 0; row < canvas->height; row++) {
			int rc = morph_tui_canvas_text_styled(canvas, cell, row, " ", &style);
			if (rc)
				return rc;
		}
	}
	return 0;
}

static const struct morph_tui_view_ops cli_composer_ops = {
	.measure = cli_composer_measure,
	.layout = cli_composer_layout,
	.draw_padding = cli_composer_draw_padding,
};

int cli_composer_update(struct morph_tui_view *view, int64_t now_ms)
{
	if (!view || view->ops != &cli_composer_ops)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct cli_composer *composer = (struct cli_composer *)view;
	int frame = cli_composer_model_animation_frame(&composer->ctx->composer, now_ms);
	if (!cli_output_color_enabled(composer->ctx) || frame >= 30)
		frame = -1;
	if (composer->frame != frame) {
		composer->frame = frame;
		morph_tui_view_invalidate(view, 0);
	}
	return 0;
}

struct morph_tui_view *cli_composer_add_borrow(struct morph_tui_builder *builder,
					       struct morph_tui_view *parent,
					       struct cli_application *ctx,
					       struct morph_tui_view **editor_borrow)
{
	if (!ctx || !editor_borrow) {
		morph_tui_builder_fail(builder, -EINVAL);
		return NULL;
	}
	*editor_borrow = NULL;
	struct morph_tui_view_options options = {
		.margin = {.top = 1}, .padding = {.top = 1, .bottom = 1}, .max_height = 8};
	if (cli_output_color_enabled(ctx))
		options.style.background = (struct morph_tui_color){MORPH_TUI_COLOR_INDEXED,
								    CLI_INPUT_BACKGROUND_INDEX};
	struct cli_composer *composer = (struct cli_composer *)morph_tui_widget_add_borrow(
		builder, parent, sizeof(*composer), &cli_composer_ops, &options);
	if (!composer)
		return NULL;
	composer->ctx = ctx;
	composer->frame = -1;
	struct morph_tui_view *view = &composer->base.view;
	composer->stack = morph_tui_widget_stack_add_borrow(builder, view,
							    MORPH_TUI_WIDGET_STACK_VERTICAL,
							    0, NULL);
	const struct morph_tui_view_options editor_options = {.max_height = 6};
	struct morph_tui_view *editor = morph_tui_widget_text_field_add_borrow(
		builder, composer->stack, "", 1024u * 1024u, 0, &editor_options);
	if (editor) {
		morph_tui_builder_fail(
			builder, morph_tui_widget_text_field_set_multiline(editor, 1));
		morph_tui_builder_fail(builder,
			morph_tui_widget_text_field_set_decoration_borrow(editor,
				cli_composer_decorate, ctx));
		morph_tui_builder_fail(builder,
			morph_tui_widget_text_field_set_atomic_range_resolver_borrow(editor,
				cli_composer_atomic_range, ctx));
	}
	*editor_borrow = editor;
	return view;
}
