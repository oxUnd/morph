#include <morph/tui/core/layout.h>
#include <morph/tui/render/draw.h>
#include "util/error.h"

static int core_layout_clamp_extent(int extent, int available)
{
	return extent < 0 ? 0 : extent > available ? available : extent;
}

struct morph_tui_core_layout_rect
morph_tui_core_layout_rect_inset(struct morph_tui_core_layout_rect area, int horizontal,
				 int vertical)
{
	int x = core_layout_clamp_extent(horizontal, area.width / 2);
	int y = core_layout_clamp_extent(vertical, area.height / 2);

	area.x += x;
	area.y += y;
	area.width -= x * 2;
	area.height -= y * 2;
	return area;
}

struct morph_tui_core_layout_rect
morph_tui_core_layout_rect_take_top(struct morph_tui_core_layout_rect *area, int height)
{
	struct morph_tui_core_layout_rect result = *area;

	result.height = core_layout_clamp_extent(height, area->height);
	area->y += result.height;
	area->height -= result.height;
	return result;
}

struct morph_tui_core_layout_rect
morph_tui_core_layout_rect_take_left(struct morph_tui_core_layout_rect *area, int width)
{
	struct morph_tui_core_layout_rect result = *area;

	result.width = core_layout_clamp_extent(width, area->width);
	area->x += result.width;
	area->width -= result.width;
	return result;
}

int morph_tui_core_layout_cell(morph_buf_t *commands, struct morph_tui_core_layout_rect area,
			       int row, int column, int width, const char *text, const char *style)
{
	if (row < 0 || column < 0 || row >= area.height || column >= area.width)
		return 0;
	width = core_layout_clamp_extent(width, area.width - column);
	return morph_tui_render_draw_cell_append(commands, area.x + area.width - 1,
						 area.y + area.height - 1, area.y + row,
						 area.x + column, width, text, style);
}
