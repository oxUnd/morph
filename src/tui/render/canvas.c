#include <morph/tui/canvas.h>
#include "util/array.h"
#include "util/error.h"
#include "util/utf8.h"
#include "util/cleanup.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct render_canvas_cell {
	morph_buf_t text;
	morph_buf_t hyperlink;
	struct morph_tui_text_style style;
	int span;
};

struct morph_tui_surface {
	morph_array_t cells, images;
	int width, height;
	int cursor_x, cursor_y, cursor_visible;
};

int morph_tui_surface_create(struct morph_tui_surface **out)
{
	if (!out)
		return -EINVAL;
	if (*out)
		return -EEXIST;
	struct morph_tui_surface *surface = calloc(1, sizeof(*surface));
	struct render_canvas_cell cell;

	if (!surface)
		return -ENOMEM;
	int rc = morph_array_init(&surface->cells, 16, sizeof(cell));
	struct morph_tui_media_placement placement;
	if (!rc)
		rc = morph_array_init(&surface->images, 4, sizeof(placement));

	if (rc) {
		morph_array_cleanup(&surface->cells);
		free(surface);
		return rc;
	}
	*out = surface;
	return 0;
}

static void render_canvas_clear_images(struct morph_tui_surface *surface)
{
	for (size_t i = 0; i < surface->images.nelts; i++) {
		struct morph_tui_media_placement *image = morph_array_borrow(&surface->images, i);
		morph_tui_media_asset_destroy(image->asset);
	}
	morph_array_clear(&surface->images);
}

void morph_tui_surface_destroy(struct morph_tui_surface *surface)
{
	if (!surface)
		return;
	for (size_t i = 0; i < surface->cells.nelts; i++) {
		struct render_canvas_cell *cell = morph_array_borrow(&surface->cells, i);

		morph_buf_cleanup(&cell->text);
		morph_buf_cleanup(&cell->hyperlink);
	}
	morph_array_cleanup(&surface->cells);
	render_canvas_clear_images(surface);
	morph_array_cleanup(&surface->images);
	free(surface);
}

void morph_tui_canvas_cursor(struct morph_tui_canvas *canvas, int x, int y)
{
	if (!canvas || !canvas->surface || x < 0 || y < 0 || x >= canvas->width ||
	    y >= canvas->height)
		return;
	int64_t column = (int64_t)x + canvas->x;
	int64_t row = (int64_t)y + canvas->y;

	if (column < canvas->left || column >= canvas->right || row < canvas->top ||
	    row >= canvas->bottom)
		return;
	canvas->surface->cursor_x = (int)column;
	canvas->surface->cursor_y = (int)row;
	canvas->surface->cursor_visible = 1;
}

int morph_tui_surface_cursor(const struct morph_tui_surface *surface, int *x, int *y)
{
	if (!surface || !x || !y)
		return 0;
	*x = surface->cursor_x;
	*y = surface->cursor_y;
	return surface->cursor_visible;
}

int morph_tui_surface_resize(struct morph_tui_surface *surface, int width, int height)
{
	if (!surface || width < 0 || height < 0)
		return -EINVAL;
	if (height && (size_t)width > SIZE_MAX / (size_t)height)
		return -EOVERFLOW;
	size_t count = (size_t)width * (size_t)height;

	if (count > surface->cells.nelts) {
		size_t added = count - surface->cells.nelts;
		struct render_canvas_cell *cells =
			morph_array_push_n_borrow(&surface->cells, added);

		if (!cells)
			return -ENOMEM;
		memset(cells, 0, added * sizeof(*cells));
	}
	surface->width = width;
	surface->height = height;
	morph_tui_surface_clear(surface);
	return 0;
}

void morph_tui_surface_clear(struct morph_tui_surface *surface)
{
	if (!surface)
		return;
	surface->cursor_visible = 0;
	render_canvas_clear_images(surface);
	for (size_t i = 0; i < surface->cells.nelts; i++) {
		struct render_canvas_cell *cell = morph_array_borrow(&surface->cells, i);

		morph_buf_reset(&cell->text);
		morph_buf_reset(&cell->hyperlink);
		cell->span = 1;
		cell->style = (struct morph_tui_text_style){0};
	}
}

void morph_tui_canvas_root_borrow(struct morph_tui_surface *surface,
				  struct morph_tui_canvas *canvas)
{
	*canvas = (struct morph_tui_canvas){
		.surface = surface,
		.width = surface->width,
		.height = surface->height,
		.right = surface->width,
		.bottom = surface->height,
	};
}

void morph_tui_canvas_child_borrow(const struct morph_tui_canvas *parent, int x, int y, int width,
				   int height, struct morph_tui_canvas *child)
{
	*child = *parent;
	int64_t left = (int64_t)parent->x + x, top = (int64_t)parent->y + y;
	int64_t right = left + width, bottom = top + height;

	if (width < 0 || height < 0 || left < INT_MIN || left > INT_MAX || top < INT_MIN ||
	    top > INT_MAX) {
		child->width = child->height = 0;
		child->right = child->left;
		child->bottom = child->top;
		return;
	}
	child->x = (int)left;
	child->y = (int)top;
	child->width = width;
	child->height = height;
	if (child->left < child->x)
		child->left = child->x;
	if (child->top < child->y)
		child->top = child->y;
	if (child->right > right)
		child->right = (int)right;
	if (child->bottom > bottom)
		child->bottom = (int)bottom;
}

static struct render_canvas_cell *render_canvas_cell(struct morph_tui_surface *surface, int x,
						     int y)
{
	return morph_array_borrow(&surface->cells, (size_t)y * (size_t)surface->width + (size_t)x);
}

static void render_canvas_erase(struct morph_tui_surface *surface, int x, int y)
{
	struct render_canvas_cell *cell = render_canvas_cell(surface, x, y);

	if (!cell->span && x > 0) {
		struct render_canvas_cell *lead = render_canvas_cell(surface, x - 1, y);

		morph_buf_reset(&lead->text);
		morph_buf_reset(&lead->hyperlink);
		lead->span = 1;
	}
	if (cell->span == 2 && x + 1 < surface->width) {
		struct render_canvas_cell *trail = render_canvas_cell(surface, x + 1, y);

		morph_buf_reset(&trail->text);
		morph_buf_reset(&trail->hyperlink);
		trail->span = 1;
	}
	morph_buf_reset(&cell->text);
	morph_buf_reset(&cell->hyperlink);
	cell->span = 1;
}

int morph_tui_text_style_equal(struct morph_tui_text_style a, struct morph_tui_text_style b)
{
	return a.attributes == b.attributes && a.foreground.kind == b.foreground.kind &&
	       a.foreground.value == b.foreground.value && a.background.kind == b.background.kind &&
	       a.background.value == b.background.value;
}

int morph_tui_text_style_valid(struct morph_tui_text_style style)
{
	if (style.attributes & ~(unsigned)(MORPH_TUI_STYLE_BOLD | MORPH_TUI_STYLE_DIM |
	    MORPH_TUI_STYLE_SELECTED | MORPH_TUI_STYLE_ITALIC | MORPH_TUI_STYLE_UNDERLINE |
	    MORPH_TUI_STYLE_STRIKE))
		return 0;
	struct morph_tui_color colors[] = {style.foreground, style.background};
	for (size_t i = 0; i < 2; i++) {
		struct morph_tui_color color = colors[i];
		if (color.kind < MORPH_TUI_COLOR_INHERIT || color.kind > MORPH_TUI_COLOR_RGB ||
		    (color.kind <= MORPH_TUI_COLOR_DEFAULT && color.value) ||
		    (color.kind == MORPH_TUI_COLOR_INDEXED && color.value > 255u) ||
		    (color.kind == MORPH_TUI_COLOR_RGB && color.value > 0xffffffu))
			return 0;
	}
	return 1;
}

int morph_tui_canvas_fill(struct morph_tui_canvas *canvas, unsigned attributes)
{
	if (!canvas)
		return -EINVAL;
	struct morph_tui_text_style style = canvas->style;
	style.attributes |= attributes;
	return morph_tui_canvas_fill_styled(canvas, &style);
}

int morph_tui_canvas_fill_styled(struct morph_tui_canvas *canvas,
				 const struct morph_tui_text_style *style)
{
	if (!canvas || !canvas->surface || !style || !morph_tui_text_style_valid(*style))
		return -EINVAL;
	for (int y = canvas->top; y < canvas->bottom; y++) {
		for (int x = canvas->left; x < canvas->right; x++) {
			render_canvas_erase(canvas->surface, x, y);
			render_canvas_cell(canvas->surface, x, y)->style = *style;
		}
	}
	return 0;
}

int morph_tui_canvas_text(struct morph_tui_canvas *canvas, int x, int y, const char *text,
			  unsigned attributes)
{
	if (!canvas)
		return -EINVAL;
	struct morph_tui_text_style style = canvas->style;
	style.attributes |= attributes;
	return morph_tui_canvas_text_styled(canvas, x, y, text, &style);
}

int morph_tui_canvas_text_styled(struct morph_tui_canvas *canvas, int x, int y, const char *text,
				 const struct morph_tui_text_style *style)
{
	return morph_tui_canvas_text_link(canvas, x, y, text, style, NULL);
}

int morph_tui_canvas_text_link(struct morph_tui_canvas *canvas, int x, int y, const char *text,
	const struct morph_tui_text_style *style, const char *uri)
{
	if (!canvas || !canvas->surface || !text || !style || !morph_tui_text_style_valid(*style))
		return -EINVAL;
	if (uri) {
		char *safe_uri MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
			uri, strlen(uri), UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
		if (!safe_uri)
			return -ENOMEM;
		if (strcmp(uri, safe_uri))
			return -EINVAL;
	}
	if (y < 0 || y >= canvas->height)
		return 0;
	y += canvas->y;
	if (y < canvas->top || y >= canvas->bottom)
		return 0;
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text), UTF8_TERMINAL_TEXT_SINGLE_LINE,
						NULL);

	if (!safe)
		return -ENOMEM;
	size_t length = strlen(safe), offset = 0;
	int last = -1, rc = 0;
	int64_t column = (int64_t)canvas->x + x;
	if (column > canvas->left && column <= canvas->right) {
		int previous = (int)column - 1;
		struct render_canvas_cell *cell = render_canvas_cell(canvas->surface, previous, y);
		if (!cell->span && previous > canvas->left) {
			previous--;
			cell = render_canvas_cell(canvas->surface, previous, y);
		}
		if (cell->text.len)
			last = previous;
	}

	while (offset < length && !rc) {
		unsigned cp;
		size_t bytes;

		if (!utf8_decode_codepoint(safe + offset, length - offset, &cp, &bytes)) {
			rc = MORPH_ERR_FORMAT;
			break;
		}
		int width = utf8_codepoint_width(cp);

		if (width < 0)
			width = 1;
		if (!width) {
			if (last >= 0) {
				struct render_canvas_cell *cell =
					render_canvas_cell(canvas->surface, last, y);

				rc = morph_buf_append(&cell->text, safe + offset, bytes);
			}
		} else if (column >= canvas->left && column + width <= canvas->right) {
			int target = (int)column;

			render_canvas_erase(canvas->surface, target, y);
			if (width == 2)
				render_canvas_erase(canvas->surface, target + 1, y);
			struct render_canvas_cell *cell =
				render_canvas_cell(canvas->surface, target, y);

			if (!cell->text.data)
				rc = morph_buf_init(&cell->text, 8);
			if (!rc)
				rc = morph_buf_append(&cell->text, safe + offset, bytes);
			if (!rc && uri && *uri)
				rc = morph_buf_puts(&cell->hyperlink, uri);
			cell->style = *style;
			cell->span = width;
			if (width == 2) {
				struct render_canvas_cell *trail =
					render_canvas_cell(canvas->surface, target + 1, y);

				trail->span = 0;
				trail->style = *style;
			}
			last = target;
		} else if (width) {
			last = -1;
		}
		column += width;
		offset += bytes;
	}
	free(safe);
	return rc;
}

int morph_tui_surface_rows(struct morph_tui_surface *surface, morph_buf_t *rows)
{
	if (!surface || !rows)
		return -EINVAL;
	morph_buf_reset(rows);
	for (int y = 0; y < surface->height; y++) {
		struct morph_tui_text_style style = {0};
		int styled = 0;
		const char *hyperlink = "";

		for (int x = 0; x < surface->width; x++) {
			struct render_canvas_cell *cell = render_canvas_cell(surface, x, y);

			if (!cell->span)
				continue;
			const char *uri = cell->hyperlink.len ? cell->hyperlink.data : "";
			if (strcmp(uri, hyperlink)) {
				if (*hyperlink)
					morph_buf_puts(rows, "\033]8;;\033\\");
				if (*uri)
					morph_buf_printf(rows, "\033]8;;%s\033\\", uri);
				hyperlink = uri;
			}
			if (!styled || !morph_tui_text_style_equal(cell->style, style)) {
				styled = 1;
				style = cell->style;
				morph_buf_puts(rows, "\033[0m");
				if (style.attributes & MORPH_TUI_STYLE_BOLD)
					morph_buf_puts(rows, "\033[1m");
				if (style.attributes & MORPH_TUI_STYLE_DIM)
					morph_buf_puts(rows, "\033[2m");
				if (style.attributes & MORPH_TUI_STYLE_SELECTED)
					morph_buf_puts(rows, "\033[7m");
				if (style.attributes & MORPH_TUI_STYLE_ITALIC)
					morph_buf_puts(rows, "\033[3m");
				if (style.attributes & MORPH_TUI_STYLE_UNDERLINE)
					morph_buf_puts(rows, "\033[4m");
				if (style.attributes & MORPH_TUI_STYLE_STRIKE)
					morph_buf_puts(rows, "\033[9m");
				struct morph_tui_color colors[] = {
					style.foreground, style.background};
				for (size_t i = 0; i < 2; i++) {
					unsigned code = i ? 48u : 38u;
					struct morph_tui_color color = colors[i];
					if (color.kind == MORPH_TUI_COLOR_INDEXED)
						morph_buf_printf(rows, "\033[%u;5;%um",
							code, color.value);
					else if (color.kind == MORPH_TUI_COLOR_RGB)
						morph_buf_printf(rows, "\033[%u;2;%u;%u;%um", code,
							(color.value >> 16u) & 255u,
							(color.value >> 8u) & 255u,
							color.value & 255u);
				}

			}
			if (cell->text.len)
				morph_buf_append(rows, cell->text.data, cell->text.len);
			else
				morph_buf_putc(rows, ' ');
		}
		if (*hyperlink)
			morph_buf_puts(rows, "\033]8;;\033\\");
		morph_buf_puts(rows, "\033[0m\n");
	}
	return rows->failed;
}

size_t morph_tui_surface_image_count(const struct morph_tui_surface *surface)
{
	return surface ? surface->images.nelts : 0;
}

const struct morph_tui_media_placement *morph_tui_surface_image_borrow(
	const struct morph_tui_surface *surface, size_t index)
{
	return surface ? morph_array_borrow(&surface->images, index) : NULL;
}

int morph_tui_canvas_image(struct morph_tui_canvas *canvas, struct morph_tui_media_asset *asset,
			   int x, int y, int width, int height)
{
	if (!canvas || !canvas->surface || !asset || width < 0 || height < 0)
		return -EINVAL;
	if (!width || !height)
		return 0;
	int64_t origin_x = (int64_t)canvas->x + x;
	int64_t origin_y = (int64_t)canvas->y + y;
	int64_t left = origin_x > canvas->left ? origin_x : canvas->left;
	int64_t top = origin_y > canvas->top ? origin_y : canvas->top;
	int64_t right = origin_x + width < canvas->right ? origin_x + width : canvas->right;
	int64_t bottom = origin_y + height < canvas->bottom ? origin_y + height : canvas->bottom;
	if (left >= right || top >= bottom)
		return 0;
	int pixels_wide = morph_tui_media_asset_width(asset);
	int pixels_high = morph_tui_media_asset_height(asset);
	int source_x = (int)((left - origin_x) * pixels_wide / width);
	int source_y = (int)((top - origin_y) * pixels_high / height);
	int end_x = (int)(((right - origin_x) * pixels_wide + width - 1) / width);
	int end_y = (int)(((bottom - origin_y) * pixels_high + height - 1) / height);
	struct morph_tui_media_placement *placement =
		morph_array_push_borrow(&canvas->surface->images);
	if (!placement)
		return -ENOMEM;
	*placement = (struct morph_tui_media_placement){
		.asset = morph_tui_media_asset_retain(asset),
		.x = (int)left, .y = (int)top,
		.width = (int)(right - left), .height = (int)(bottom - top),
		.source_x = source_x, .source_y = source_y,
		.source_width = end_x - source_x, .source_height = end_y - source_y};
	return 0;
}
