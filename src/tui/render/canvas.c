#include <morph/tui/canvas.h>
#include "util/array.h"
#include "util/error.h"
#include "util/utf8.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct render_canvas_cell {
	morph_buf_t text;
	unsigned style;
	int span;
};

struct morph_tui_surface {
	morph_array_t cells;
	int width, height;
	int cursor_x, cursor_y, cursor_visible;
};

int morph_tui_surface_create(struct morph_tui_surface **out)
{
	if (!out)
		return -EINVAL;
	*out = NULL;
	struct morph_tui_surface *surface = calloc(1, sizeof(*surface));
	struct render_canvas_cell cell;

	if (!surface)
		return -ENOMEM;
	int rc = morph_array_init(&surface->cells, 16, sizeof(cell));

	if (rc) {
		free(surface);
		return rc;
	}
	*out = surface;
	return 0;
}

void morph_tui_surface_destroy(struct morph_tui_surface *surface)
{
	if (!surface)
		return;
	for (size_t i = 0; i < surface->cells.nelts; i++) {
		struct render_canvas_cell *cell = morph_array_get(&surface->cells, i);

		morph_buf_cleanup(&cell->text);
	}
	morph_array_cleanup(&surface->cells);
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
		struct render_canvas_cell *cells = morph_array_push_n(&surface->cells, added);

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
	for (size_t i = 0; i < surface->cells.nelts; i++) {
		struct render_canvas_cell *cell = morph_array_get(&surface->cells, i);

		morph_buf_reset(&cell->text);
		cell->span = 1;
		cell->style = 0;
	}
}

void morph_tui_canvas_root(struct morph_tui_surface *surface, struct morph_tui_canvas *canvas)
{
	*canvas = (struct morph_tui_canvas){
		.surface = surface,
		.width = surface->width,
		.height = surface->height,
		.right = surface->width,
		.bottom = surface->height,
	};
}

void morph_tui_canvas_child(const struct morph_tui_canvas *parent, int x, int y, int width,
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
	return morph_array_get(&surface->cells, (size_t)y * (size_t)surface->width + (size_t)x);
}

static void render_canvas_erase(struct morph_tui_surface *surface, int x, int y)
{
	struct render_canvas_cell *cell = render_canvas_cell(surface, x, y);

	if (!cell->span && x > 0) {
		struct render_canvas_cell *lead = render_canvas_cell(surface, x - 1, y);

		morph_buf_reset(&lead->text);
		lead->span = 1;
	}
	if (cell->span == 2 && x + 1 < surface->width) {
		struct render_canvas_cell *trail = render_canvas_cell(surface, x + 1, y);

		morph_buf_reset(&trail->text);
		trail->span = 1;
	}
	morph_buf_reset(&cell->text);
	cell->span = 1;
}

int morph_tui_canvas_fill(struct morph_tui_canvas *canvas, unsigned style)
{
	if (!canvas || !canvas->surface)
		return -EINVAL;
	for (int y = canvas->top; y < canvas->bottom; y++) {
		for (int x = canvas->left; x < canvas->right; x++) {
			render_canvas_erase(canvas->surface, x, y);
			render_canvas_cell(canvas->surface, x, y)->style = style;
		}
	}
	return 0;
}

int morph_tui_canvas_text(struct morph_tui_canvas *canvas, int x, int y, const char *text,
			  unsigned style)
{
	if (!canvas || !canvas->surface || !text)
		return -EINVAL;
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
			cell->style = style;
			cell->span = width;
			if (width == 2) {
				struct render_canvas_cell *trail =
					render_canvas_cell(canvas->surface, target + 1, y);

				trail->span = 0;
				trail->style = style;
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
		unsigned style = UINT_MAX;

		for (int x = 0; x < surface->width; x++) {
			struct render_canvas_cell *cell = render_canvas_cell(surface, x, y);

			if (!cell->span)
				continue;
			if (cell->style != style) {
				style = cell->style;
				morph_buf_puts(rows, "\033[0m");
				if (style & MORPH_TUI_STYLE_BOLD)
					morph_buf_puts(rows, "\033[1m");
				if (style & MORPH_TUI_STYLE_DIM)
					morph_buf_puts(rows, "\033[2m");
				if (style & MORPH_TUI_STYLE_SELECTED)
					morph_buf_puts(rows, "\033[7m");
			}
			if (cell->text.len)
				morph_buf_append(rows, cell->text.data, cell->text.len);
			else
				morph_buf_putc(rows, ' ');
		}
		morph_buf_puts(rows, "\033[0m\n");
	}
	return rows->failed;
}
