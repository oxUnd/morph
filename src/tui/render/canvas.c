#include <morph/tui/canvas.h>
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
#include "nb/cleanup.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct render_canvas_cell {
	nb_buf text;
	nb_buf hyperlink;
	struct morph_tui_text_style style;
	int span;
};

struct morph_tui_surface {
	nb_vec cells, images;
	int width, height;
	int cursor_x, cursor_y, cursor_visible;
};

int morph_tui_surface_create(struct morph_tui_surface **out)
{
	if (!out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	struct morph_tui_surface *surface = calloc(1, sizeof(*surface));
	struct render_canvas_cell cell;

	if (!surface)
		return NB_ERR_NOMEM;
	int rc = (nb_vec_init(&surface->cells, sizeof(cell)),
		  nb_vec_reserve(&surface->cells, 16) ? 0 : NB_ERR_NOMEM);
	struct morph_tui_media_placement placement;
	if (!rc)
		rc = (nb_vec_init(&surface->images, sizeof(placement)),
		      nb_vec_reserve(&surface->images, 4) ? 0 : NB_ERR_NOMEM);

	if (rc) {
		nb_vec_cleanup(&surface->cells);
		free(surface);
		return rc;
	}
	*out = surface;
	return 0;
}

static void render_canvas_clear_images(struct morph_tui_surface *surface)
{
	for (size_t i = 0; i < surface->images.len; i++) {
		struct morph_tui_media_placement *image = nb_vec_at(&surface->images, i);
		morph_tui_media_asset_destroy(image->asset);
	}
	nb_vec_clear(&surface->images);
}

void morph_tui_surface_destroy(struct morph_tui_surface *surface)
{
	if (!surface)
		return;
	for (size_t i = 0; i < surface->cells.len; i++) {
		struct render_canvas_cell *cell = nb_vec_at(&surface->cells, i);

		nb_buf_cleanup(&cell->text);
		nb_buf_cleanup(&cell->hyperlink);
	}
	nb_vec_cleanup(&surface->cells);
	render_canvas_clear_images(surface);
	nb_vec_cleanup(&surface->images);
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
		return NB_ERR_INVAL;
	if (height && (size_t)width > SIZE_MAX / (size_t)height)
		return NB_ERR_RANGE;
	size_t count = (size_t)width * (size_t)height;

	if (count > surface->cells.len) {
		size_t added = count - surface->cells.len;
		struct render_canvas_cell *cells = nb_vec_push_n(&surface->cells, added);

		if (!cells)
			return NB_ERR_NOMEM;
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
	for (size_t i = 0; i < surface->cells.len; i++) {
		struct render_canvas_cell *cell = nb_vec_at(&surface->cells, i);

		nb_buf_clear(&cell->text);
		nb_buf_clear(&cell->hyperlink);
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
	return nb_vec_at(&surface->cells, (size_t)y * (size_t)surface->width + (size_t)x);
}

static int render_canvas_selection_range(struct morph_tui_surface *surface,
					 int *x1, int *y1, int *x2, int *y2)
{
	if (!surface->width || !surface->height)
		return 0;
	*x1 = *x1 < 0 ? 0 : *x1 >= surface->width ? surface->width - 1 : *x1;
	*x2 = *x2 < 0 ? 0 : *x2 >= surface->width ? surface->width - 1 : *x2;
	*y1 = *y1 < 0 ? 0 : *y1 >= surface->height ? surface->height - 1 : *y1;
	*y2 = *y2 < 0 ? 0 : *y2 >= surface->height ? surface->height - 1 : *y2;
	if (*y1 > *y2 || (*y1 == *y2 && *x1 > *x2)) {
		int x = *x1, y = *y1;

		*x1 = *x2;
		*y1 = *y2;
		*x2 = x;
		*y2 = y;
	}
	if (*x1 > 0 && !render_canvas_cell(surface, *x1, *y1)->span)
		(*x1)--;
	if (*x2 + 1 < surface->width && render_canvas_cell(surface, *x2, *y2)->span == 2)
		(*x2)++;
	return 1;
}

int morph_tui_surface_selection_text(struct morph_tui_surface *surface, int x1, int y1, int x2,
				     int y2, nb_buf *text)
{
	if (!surface || !text)
		return NB_ERR_INVAL;
	nb_buf_clear(text);
	if (!render_canvas_selection_range(surface, &x1, &y1, &x2, &y2))
		return 0;
	for (int y = y1; y <= y2; y++) {
		int start = y == y1 ? x1 : 0;
		int end = y == y2 ? x2 : surface->width - 1;
		size_t row_start = text->len;

		for (int x = start; x <= end; x++) {
			struct render_canvas_cell *cell = render_canvas_cell(surface, x, y);

			if (!cell->span)
				continue;
			if (cell->text.len)
				nb_buf_append(text, cell->text.ptr, cell->text.len);
			else
				nb_buf_append_char(text, ' ');
		}
		while (text->len > row_start && text->ptr[text->len - 1] == ' ')
			text->ptr[--text->len] = '\0';
		if (y != y2)
			nb_buf_append_char(text, '\n');
	}
	return nb_buf_status(text);
}

int morph_tui_surface_selection_style(struct morph_tui_surface *surface,
				      int x1, int y1, int x2, int y2, unsigned attributes)
{
	struct morph_tui_text_style style = {.attributes = attributes};

	if (!surface || !morph_tui_text_style_valid(style))
		return NB_ERR_INVAL;
	if (!render_canvas_selection_range(surface, &x1, &y1, &x2, &y2))
		return 0;
	for (int y = y1; y <= y2; y++) {
		int start = y == y1 ? x1 : 0;
		int end = y == y2 ? x2 : surface->width - 1;

		for (int x = start; x <= end; x++)
			render_canvas_cell(surface, x, y)->style.attributes |= attributes;
	}
	return 0;
}

static void render_canvas_erase(struct morph_tui_surface *surface, int x, int y)
{
	struct render_canvas_cell *cell = render_canvas_cell(surface, x, y);

	if (!cell->span && x > 0) {
		struct render_canvas_cell *lead = render_canvas_cell(surface, x - 1, y);

		nb_buf_clear(&lead->text);
		nb_buf_clear(&lead->hyperlink);
		lead->span = 1;
	}
	if (cell->span == 2 && x + 1 < surface->width) {
		struct render_canvas_cell *trail = render_canvas_cell(surface, x + 1, y);

		nb_buf_clear(&trail->text);
		nb_buf_clear(&trail->hyperlink);
		trail->span = 1;
	}
	nb_buf_clear(&cell->text);
	nb_buf_clear(&cell->hyperlink);
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
		return NB_ERR_INVAL;
	struct morph_tui_text_style style = canvas->style;
	style.attributes |= attributes;
	return morph_tui_canvas_fill_styled(canvas, &style);
}

int morph_tui_canvas_fill_styled(struct morph_tui_canvas *canvas,
				 const struct morph_tui_text_style *style)
{
	if (!canvas || !canvas->surface || !style || !morph_tui_text_style_valid(*style))
		return NB_ERR_INVAL;
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
		return NB_ERR_INVAL;
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
		return NB_ERR_INVAL;
	if (uri) {
		char *safe_uri NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
			uri, strlen(uri), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
		if (!safe_uri)
			return NB_ERR_NOMEM;
		if (strcmp(uri, safe_uri))
			return NB_ERR_INVAL;
	}
	if (y < 0 || y >= canvas->height)
		return 0;
	y += canvas->y;
	if (y < canvas->top || y >= canvas->bottom)
		return 0;
	char *safe = morph_terminal_sanitize_dup(text, strlen(text),
						 MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);

	if (!safe)
		return NB_ERR_NOMEM;
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

		if (!nb_utf8_read(safe + offset, length - offset, &cp, &bytes)) {
			rc = NB_ERR_FORMAT;
			break;
		}
		int width = nb_utf8_width_cp(cp);

		if (width < 0)
			width = 1;
		if (!width) {
			if (last >= 0) {
				struct render_canvas_cell *cell =
					render_canvas_cell(canvas->surface, last, y);

				rc = (nb_buf_append(&cell->text, safe + offset, bytes)
					      ? 0
					      : NB_ERR_NOMEM);
			}
		} else if (column >= canvas->left && column + width <= canvas->right) {
			int target = (int)column;

			render_canvas_erase(canvas->surface, target, y);
			if (width == 2)
				render_canvas_erase(canvas->surface, target + 1, y);
			struct render_canvas_cell *cell =
				render_canvas_cell(canvas->surface, target, y);

			if (!cell->text.ptr)
				rc = (nb_buf_init(&cell->text),
				      nb_buf_reserve(&cell->text, 8) ? 0 : NB_ERR_NOMEM);
			if (!rc)
				rc = (nb_buf_append(&cell->text, safe + offset, bytes)
					      ? 0
					      : NB_ERR_NOMEM);
			if (!rc && uri && *uri)
				rc = (nb_buf_append_cstr(&cell->hyperlink, uri) ? 0 : NB_ERR_NOMEM);
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

int morph_tui_surface_rows(struct morph_tui_surface *surface, nb_buf *rows)
{
	if (!surface || !rows)
		return NB_ERR_INVAL;
	nb_buf_clear(rows);
	for (int y = 0; y < surface->height; y++) {
		struct morph_tui_text_style style = {0};
		int styled = 0;
		const char *hyperlink = "";

		for (int x = 0; x < surface->width; x++) {
			struct render_canvas_cell *cell = render_canvas_cell(surface, x, y);

			if (!cell->span)
				continue;
			const char *uri = cell->hyperlink.len ? cell->hyperlink.ptr : "";
			if (strcmp(uri, hyperlink)) {
				if (*hyperlink)
					nb_buf_append_cstr(rows, "\033]8;;\033\\");
				if (*uri)
					nb_buf_printf(rows, "\033]8;;%s\033\\", uri);
				hyperlink = uri;
			}
			if (!styled || !morph_tui_text_style_equal(cell->style, style)) {
				styled = 1;
				style = cell->style;
				nb_buf_append_cstr(rows, "\033[0m");
				if (style.attributes & MORPH_TUI_STYLE_BOLD)
					nb_buf_append_cstr(rows, "\033[1m");
				if (style.attributes & MORPH_TUI_STYLE_DIM)
					nb_buf_append_cstr(rows, "\033[2m");
				if (style.attributes & MORPH_TUI_STYLE_SELECTED)
					nb_buf_append_cstr(rows, "\033[7m");
				if (style.attributes & MORPH_TUI_STYLE_ITALIC)
					nb_buf_append_cstr(rows, "\033[3m");
				if (style.attributes & MORPH_TUI_STYLE_UNDERLINE)
					nb_buf_append_cstr(rows, "\033[4m");
				if (style.attributes & MORPH_TUI_STYLE_STRIKE)
					nb_buf_append_cstr(rows, "\033[9m");
				struct morph_tui_color colors[] = {
					style.foreground, style.background};
				for (size_t i = 0; i < 2; i++) {
					unsigned code = i ? 48u : 38u;
					struct morph_tui_color color = colors[i];
					if (color.kind == MORPH_TUI_COLOR_INDEXED)
						nb_buf_printf(rows, "\033[%u;5;%um", code,
							      color.value);
					else if (color.kind == MORPH_TUI_COLOR_RGB)
						nb_buf_printf(rows, "\033[%u;2;%u;%u;%um", code,
							      (color.value >> 16u) & 255u,
							      (color.value >> 8u) & 255u,
							      color.value & 255u);
				}

			}
			if (cell->text.len)
				nb_buf_append(rows, cell->text.ptr, cell->text.len);
			else
				nb_buf_append_char(rows, ' ');
		}
		if (*hyperlink)
			nb_buf_append_cstr(rows, "\033]8;;\033\\");
		nb_buf_append_cstr(rows, "\033[0m\n");
	}
	return nb_buf_status(rows);
}

size_t morph_tui_surface_image_count(const struct morph_tui_surface *surface)
{
	return surface ? surface->images.len : 0;
}

const struct morph_tui_media_placement *morph_tui_surface_image_borrow(
	const struct morph_tui_surface *surface, size_t index)
{
	return surface ? nb_vec_at(&surface->images, index) : NULL;
}

int morph_tui_canvas_image(struct morph_tui_canvas *canvas, struct morph_tui_media_asset *asset,
			   int x, int y, int width, int height)
{
	if (!canvas || !canvas->surface || !asset || width < 0 || height < 0)
		return NB_ERR_INVAL;
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
	struct morph_tui_media_placement *placement = nb_vec_push(&canvas->surface->images);
	if (!placement)
		return NB_ERR_NOMEM;
	*placement = (struct morph_tui_media_placement){
		.asset = morph_tui_media_asset_retain(asset),
		.x = (int)left, .y = (int)top,
		.width = (int)(right - left), .height = (int)(bottom - top),
		.source_x = source_x, .source_y = source_y,
		.source_width = end_x - source_x, .source_height = end_y - source_y};
	return 0;
}
