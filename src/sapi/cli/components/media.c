#include "sapi/cli/components/media.h"
#include <nb/err.h>
#include "sapi/cli/adapters/media/renderer.h"
#include "sapi/cli/presentation/messages.h"
#include "render/video.h"
#include "sapi/cli/adapters/media/asset.h"
#include <nb/cleanup.h>
#include <nb/utf8.h>
#include <nothing/tui/terminal/text.h>
#include <nothing/tui/views/image_view.h>
#include <nothing/tui/views/text_view.h>
#include <nothing/tui/view_controller.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

struct cli_media {
	struct nt_view base;
	struct nt_view *image, *caption;
	int pixels_wide, pixels_high, columns, rows;
};

static int cli_media_measure(struct nt_view *view, int width,
	struct nt_view_size *size)
{
	struct cli_media *media = (struct cli_media *)view;
	media->columns = 0;
	media->rows = 0;
	if (media->image && width > 0) {
		struct nt_view *root = nt_view_controller_root_view(nt_view_controller(view));
		int limit = root ? nt_view_frame(root).height : 0;
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

	struct nt_view_size caption = {0};
	int rc = media->caption ? nt_view_measure(media->caption, width, &caption) : 0;
	if (!rc && media->rows > INT_MAX - caption.height)
		return NB_ERR_RANGE;

	if (!rc)
		*size = (struct nt_view_size){width, media->rows + caption.height};

	return rc;
}

static int cli_media_layout(struct nt_view *view)
{
	struct cli_media *media = (struct cli_media *)view;
	struct nt_view_rect rect = nt_view_content_rect(view);
	int rc = 0;
	if (media->image)
		rc = nt_view_arrange(media->image, (struct nt_view_rect){
			rect.x, rect.y, media->columns, media->rows});

	if (!rc && media->caption) {
		rect.y += media->rows;
		rect.height = rect.height > media->rows ? rect.height - media->rows : 0;
		rc = nt_view_arrange(media->caption, rect);
	}

	return rc;
}

static void cli_media_asset_cleanup(struct nt_media_asset **owner)
{
	nt_media_asset_release(*owner);
	*owner = NULL;
}

struct nt_view *cli_media_add(struct nt_builder *builder, struct nt_view *parent, int styled,
			      const char *type, const char *path,
			      const struct nt_view_options *options)
{
	if (!type || !path || !*path || (strcmp(type, "image") && strcmp(type, "video"))) {
		nt_builder_fail(builder, NB_ERR_INVAL);
		return NULL;
	}

	int video = !strcmp(type, "video");
	struct nt_media_asset *asset NB_CLEANUP(cli_media_asset_cleanup) = NULL;
	if (styled && cli_media_native_available())
		(void)cli_renderer_media_asset_create_copy(type, path, &asset);

	static const struct nt_view_class ops = {.superclass = &nt_view_base_class,
						 .instance_size = sizeof(struct cli_media),
						 .kind = "cli_media",
						 .measure = cli_media_measure,
						 .layout = cli_media_layout};
	struct cli_media *media = (struct cli_media *)nt_view_add(
		builder, parent, &ops, options);
	if (!media)
		return NULL;

	struct nt_view *view = &media->base;
	if (asset) {
		media->pixels_wide = nt_media_asset_width(asset);
		media->pixels_high = nt_media_asset_height(asset);
		media->image = nt_image_view_add(builder, view, asset, NULL);
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
			char *safe NB_CLEANUP(nb_charp_cleanup) = nt_sanitize_dup(
				path, strlen(path), NT_TEXT_SINGLE_LINE, NULL);
			rc = safe ? (nb_buf_printf(&caption, "(image: %s)", safe) ? 0
										  : NB_ERR_NOMEM)
				  : NB_ERR_NOMEM;
		}

		nt_builder_fail(builder, rc);
		media->caption = nt_text_view_add(builder, view, "", NULL);
		if (media->caption) {
			nt_view_set_focusable(media->caption, 0);
			if (!rc)
				nt_builder_fail(builder, cli_messages_set_rendered_view(
								 media->caption,
								 nb_buf_cstr(&caption), styled));
		}
	}

	return view;
}
