#include "sapi/cli/components/composer.h"
#include <nb/err.h>
#include "sapi/cli/application.h"
#include "sapi/cli/models/composer.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include <nothing/tui/component_state.h>
#include <nothing/tui/controls/text_field.h>
#include <errno.h>
#include <limits.h>

struct cli_composer {
	struct nt_view base;
	struct nt_view *editor;
	struct cli_application *ctx;
	struct nt_component_state *frame;
};

static int cli_composer_decorate(void *data, const char *text, size_t offset,
				 struct nt_text_style *style)
{
	struct cli_application *ctx = data;
	if (!cli_output_color_enabled(ctx) || offset > INT_MAX)
		return 0;
	int start, end;
	if (cli_composer_model_image_span(&ctx->composer, text, (int)offset, 0, &start, &end)) {
		style->foreground = (struct nt_color){NT_COLOR_INDEXED, 153};
		style->background = (struct nt_color){NT_COLOR_INDEXED, 24};
	}
	return 0;
}

static int cli_composer_atomic_range(void *data, const char *text, size_t cursor, int backward,
				     size_t *start, size_t *end)
{
	struct cli_application *ctx = data;
	if (cursor > INT_MAX)
		return NB_ERR_NOT_FOUND;
	int first, last;
	if (!cli_composer_model_image_span(&ctx->composer, text, (int)cursor, backward, &first,
					   &last))
		return NB_ERR_NOT_FOUND;
	*start = (size_t)first;
	*end = (size_t)last;
	return 0;
}

static int cli_composer_measure(struct nt_view *view, int width, struct nt_view_size *size)
{
	return nt_view_measure(((struct cli_composer *)view)->editor, width, size);
}

static int cli_composer_layout(struct nt_view *view)
{
	return nt_view_arrange(((struct cli_composer *)view)->editor, nt_view_content_rect(view));
}

static int cli_composer_draw_padding(struct nt_view *view, enum nt_view_edge edge,
				     struct nt_canvas *canvas)
{
	struct cli_composer *composer = (struct cli_composer *)view;
	if (cli_output_color_enabled(composer->ctx)) {
		int rc = nt_canvas_fill_styled(canvas, &canvas->style);
		if (rc)
			return rc;
	}
	int frame = (int)nt_component_state_get_int(composer->frame);
	if (frame < 0 || frame >= 30 || (edge != NT_VIEW_EDGE_TOP && edge != NT_VIEW_EDGE_BOTTOM))
		return 0;
	int center = (int)((int64_t)frame * canvas->width / 30);
	for (int cell = 0; cell < canvas->width; cell++) {
		int distance = cell > center ? cell - center : center - cell;
		if (distance >= 8)
			continue;
		unsigned color = distance < 2	? 141u
				 : distance < 4 ? 98u
				 : distance < 6 ? 54u
						: 17u;
		struct nt_text_style style = {.background = {NT_COLOR_INDEXED, color}};
		for (int row = 0; row < canvas->height; row++) {
			int rc = nt_canvas_text_styled(canvas, cell, row, " ", &style);
			if (rc)
				return rc;
		}
	}
	return 0;
}

static const struct nt_view_class cli_composer_class = {
	.superclass = &nt_view_base_class,
	.instance_size = sizeof(struct cli_composer),
	.kind = "cli_composer",
	.measure = cli_composer_measure,
	.layout = cli_composer_layout,
	.draw_padding = cli_composer_draw_padding,
};

int cli_composer_fit(struct nt_view *view, int width, int height, int preceding_separator)
{
	if (!view || view->type != &cli_composer_class || width < 0 || height < 0)
		return NB_ERR_INVAL;
	struct cli_composer *composer = (struct cli_composer *)view;
	struct nt_view_size editor;
	int rc = nt_view_measure(composer->editor, width, &editor);
	if (rc)
		return rc;
	int spacing = height > editor.height ? height - editor.height : 0;
	rc = nt_view_set_margin(
		view, (struct nt_view_insets){.top = spacing >= 3 && !preceding_separator ? 1 : 0});
	if (!rc)
		rc = nt_view_set_padding(view,
					 (struct nt_view_insets){.top = spacing >= 2 ? 1 : 0,
								 .bottom = spacing >= 1 ? 1 : 0});
	return rc;
}

int cli_composer_update(struct nt_view *view, int64_t now_ms)
{
	if (!view || view->type != &cli_composer_class)
		return NB_ERR_INVAL;
	if (!nt_view_is_owner_thread(view))
		return NB_ERR_NOT_PERMITTED;
	struct cli_composer *composer = (struct cli_composer *)view;
	int frame = cli_composer_model_animation_frame(&composer->ctx->composer, now_ms);
	if (!cli_output_color_enabled(composer->ctx) || frame >= 30)
		frame = -1;
	if (frame == nt_component_state_get_int(composer->frame))
		return 0;
	int rc = nt_component_state_set_int(composer->frame, frame);
	if (!rc)
		nt_view_invalidate(view, 0);
	return rc;
}

struct nt_view *cli_composer_add_borrow(struct nt_builder *builder, struct nt_view *parent,
					struct cli_application *ctx, struct nt_view **editor_borrow)
{
	if (!ctx || !editor_borrow) {
		nt_builder_fail(builder, NB_ERR_INVAL);
		return NULL;
	}
	*editor_borrow = NULL;
	struct nt_view_options options = {
		.margin = {.top = 1}, .padding = {.top = 1, .bottom = 1}, .max_height = 8};
	if (cli_output_color_enabled(ctx))
		options.style.background =
			(struct nt_color){NT_COLOR_INDEXED, CLI_INPUT_BACKGROUND_INDEX};
	struct cli_composer *composer =
		(struct cli_composer *)nt_view_add(builder, parent, &cli_composer_class, &options);
	if (!composer)
		return NULL;
	composer->ctx = ctx;
	struct nt_view *view = &composer->base;
	nt_builder_fail(builder, nt_view_use_state_int(view, -1, &composer->frame));
	const struct nt_view_options editor_options = {.max_height = 6};
	struct nt_view *editor = nt_text_field_add(
		builder, view, &(struct nt_text_field_options){.limit = 1024u * 1024u},
		&editor_options);
	composer->editor = editor;
	if (editor) {
		nt_builder_fail(builder, nt_text_field_set_multiline(editor, 1));
		nt_builder_fail(builder,
				nt_text_field_set_decoration(editor, cli_composer_decorate, ctx));
		nt_builder_fail(builder, nt_text_field_set_atomic_range_resolver(
						 editor, cli_composer_atomic_range, ctx));
	}
	*editor_borrow = editor;
	return view;
}
