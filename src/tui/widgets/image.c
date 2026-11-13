#include <morph/tui/widgets/image.h>
#include "util/error.h"

#include <limits.h>

struct widget_image {
	struct morph_tui_widget base;
	struct morph_tui_media_asset *asset;
};

static int widget_image_measure(struct morph_tui_view *view, int width,
				struct morph_tui_view_size *size)
{
	struct widget_image *image = (struct widget_image *)view;
	int pixels_wide = morph_tui_media_asset_width(image->asset);
	int pixels_high = morph_tui_media_asset_height(image->asset);
	if (!pixels_wide || !pixels_high) {
		*size = (struct morph_tui_view_size){0};
		return 0;
	}
	int columns = (pixels_wide + 7) / 8;
	if (columns > width)
		columns = width;
	const struct morph_tui_view_options options = morph_tui_view_get_options(view);
	if (options.width)
		columns = options.width;
	if (columns < options.min_width)
		columns = options.min_width;
	if (options.max_width && columns > options.max_width)
		columns = options.max_width;
	int64_t rows = ((int64_t)pixels_high * columns + (int64_t)pixels_wide * 2 - 1) /
		((int64_t)pixels_wide * 2);
	if (rows > INT_MAX)
		return -EOVERFLOW;
	*size = (struct morph_tui_view_size){columns, rows > 0 ? (int)rows : 1};
	return 0;
}

static int widget_image_draw(struct morph_tui_view *view, struct morph_tui_canvas *canvas)
{
	struct widget_image *image = (struct widget_image *)view;
	return image->asset ? morph_tui_canvas_image(canvas, image->asset, 0, 0,
		canvas->width, canvas->height) : 0;
}

static void widget_image_destroy(struct morph_tui_view *view)
{
	morph_tui_media_asset_destroy(((struct widget_image *)view)->asset);
}

static const struct morph_tui_view_ops widget_image_ops = {
	.measure = widget_image_measure,
	.draw = widget_image_draw,
	.destroy = widget_image_destroy,
};

int morph_tui_widget_image_set_asset_borrow(struct morph_tui_view *view,
					   struct morph_tui_media_asset *asset)
{
	if (!view || view->ops != &widget_image_ops)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct widget_image *image = (struct widget_image *)view;
	if (image->asset != asset) {
		morph_tui_media_asset_retain(asset);
		morph_tui_media_asset_destroy(image->asset);
		image->asset = asset;
		morph_tui_view_invalidate(view, 1);
	}
	return 0;
}

struct morph_tui_view *morph_tui_widget_image_add_borrow(struct morph_tui_builder *builder,
	struct morph_tui_view *parent, struct morph_tui_media_asset *asset,
	const struct morph_tui_view_options *options)
{
	struct widget_image *image = (struct widget_image *)morph_tui_widget_add_borrow(
		builder, parent, sizeof(*image), &widget_image_ops, options);
	if (!image)
		return NULL;
	image->asset = morph_tui_media_asset_retain(asset);
	return &image->base.view;
}
