#include "sapi/cli/widgets/media.h"
#include "nb/err.h"
#include "sapi/cli/application.h"
#include "sapi/cli/adapters/media/renderer.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/output.h"
#include "render/video.h"
#include "nb/cleanup.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
#include <morph/tui/widgets/image.h>
#include <morph/tui/widgets/text_view.h>
#include <morph/tui/window.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

struct cli_media {
	struct morph_tui_widget base;
	struct cli_application *ctx;
	struct morph_tui_view *image, *caption;
	int pixels_wide, pixels_high, columns, rows;
};

static int cli_media_measure(struct morph_tui_view *view, int width,
	struct morph_tui_view_size *size)
{
	struct cli_media *media = (struct cli_media *)view;
	media->columns = 0;
	media->rows = 0;
	if (media->image && width > 0) {
		struct morph_tui_view *root = morph_tui_window_root_borrow(
			morph_tui_application_window_borrow(&media->ctx->base));
		int limit = root ? morph_tui_view_frame(root).height : 0;
		if (limit < 1)
			limit = 24;
		int columns = width / 2;
		if (columns < 1)
			columns = 1;
		int64_t numerator = (int64_t)media->pixels_high * columns;
		int64_t denominator = (int64_t)media->pixels_wide * 2;
		int rows = (int)((numerator + denominator - 1) / denominator);
		if (rows > limit) {
			columns = (int)(((int64_t)media->pixels_wide * 2 * limit +
				media->pixels_high - 1) / media->pixels_high);
			rows = limit;
		}
		media->columns = columns;
		media->rows = rows > 0 ? rows : 1;
	}
	struct morph_tui_view_size caption = {0};
	int rc = media->caption ? morph_tui_view_measure(media->caption, width, &caption) : 0;
	if (!rc && media->rows > INT_MAX - caption.height)
		return NB_ERR_RANGE;
	if (!rc)
		*size = (struct morph_tui_view_size){width, media->rows + caption.height};
	return rc;
}

static int cli_media_layout(struct morph_tui_view *view)
{
	struct cli_media *media = (struct cli_media *)view;
	struct morph_tui_view_rect rect = morph_tui_view_content_rect(view);
	int rc = 0;
	if (media->image)
		rc = morph_tui_view_arrange(media->image, (struct morph_tui_view_rect){
			rect.x, rect.y, media->columns, media->rows});
	if (!rc && media->caption) {
		rect.y += media->rows;
		rect.height = rect.height > media->rows ? rect.height - media->rows : 0;
		rc = morph_tui_view_arrange(media->caption, rect);
	}
	return rc;
}

static void cli_media_asset_cleanup(struct morph_tui_media_asset **owner)
{
	morph_tui_media_asset_destroy(*owner);
	*owner = NULL;
}

struct morph_tui_view *cli_media_add_borrow(struct morph_tui_builder *builder,
	struct morph_tui_view *parent, struct cli_application *ctx, const char *type,
	const char *path, const struct morph_tui_view_options *options)
{
	if (!ctx || !type || !path || !*path || (strcmp(type, "image") && strcmp(type, "video"))) {
		morph_tui_builder_fail(builder, NB_ERR_INVAL);
		return NULL;
	}
	int video = !strcmp(type, "video");
	struct morph_tui_media_asset *asset NB_CLEANUP(cli_media_asset_cleanup) = NULL;
	if (cli_output_color_enabled(ctx) && morph_tui_media_protocol_available())
		(void)cli_renderer_media_asset_create_copy(type, path, &asset);
	static const struct morph_tui_view_ops ops = {
		.measure = cli_media_measure, .layout = cli_media_layout};
	struct cli_media *media = (struct cli_media *)morph_tui_widget_add_borrow(
		builder, parent, sizeof(*media), &ops, options);
	if (!media)
		return NULL;
	media->ctx = ctx;
	struct morph_tui_view *view = &media->base.view;
	if (asset) {
		media->pixels_wide = morph_tui_media_asset_width(asset);
		media->pixels_high = morph_tui_media_asset_height(asset);
		media->image = morph_tui_widget_image_add_borrow(builder, view, asset, NULL);
	}
	if (video || !asset) {
		nb_buf caption NB_CLEANUP(nb_buf_cleanup) = {0};
		int rc;
		if (video) {
			nb_buf url NB_CLEANUP(nb_buf_cleanup) = {0};
			rc = video_file_url_append(path, &url);
			if (!rc)
				rc = (nb_buf_printf(&caption,
						    "\033]8;;%s\033\\▶ Open video\033]8;;\033\\",
						    nb_buf_cstr(&url))
					      ? 0
					      : NB_ERR_NOMEM);
		} else {
			char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
				path, strlen(path), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
			rc = safe ? (nb_buf_printf(&caption, "(image: %s)", safe) ? 0
										  : NB_ERR_NOMEM)
				  : NB_ERR_NOMEM;
		}
		morph_tui_builder_fail(builder, rc);
		media->caption = morph_tui_widget_text_view_add_borrow(builder, view, "", NULL);
		if (media->caption) {
			morph_tui_view_set_focusable(media->caption, 0);
			morph_tui_builder_fail(builder,
				morph_tui_widget_text_view_set_content_sizing(media->caption, 1));
			if (!rc)
				morph_tui_builder_fail(
					builder, cli_messages_set_rendered_view(
							 media->caption, nb_buf_cstr(&caption),
							 cli_output_color_enabled(ctx)));
		}
	}
	return view;
}
