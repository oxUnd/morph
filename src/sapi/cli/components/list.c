#include "sapi/cli/components/list.h"
#include <nothing/tui/controls/list_view.h>

static void cli_list_selection(void *data, size_t index, int selected, struct nt_text_style *style)
{
	(void)index;
	*style = (struct nt_text_style){0};
	if (selected && nt_view_is_focused(data)) {
		style->attributes = NT_STYLE_BOLD;
		style->foreground = (struct nt_color){NT_COLOR_INDEXED, 6};
	}
}

struct nt_view *cli_list_add(struct nt_builder *builder, struct nt_view *parent,
			     const struct nt_view_options *options)
{
	struct nt_view *list = nt_list_view_add(builder, parent, NULL, 0, 0, options);
	if (list)
		nt_builder_fail(builder,
				nt_list_view_set_row_style(list, cli_list_selection, list));

	return list;
}
