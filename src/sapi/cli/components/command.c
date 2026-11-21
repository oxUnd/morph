#include "sapi/cli/components/command.h"

#include "sapi/cli/presentation/style.h"

#include <nothing/tui/views/label.h>
#include <nothing/tui/views/stack_view.h>

static int cli_command_top_margin(struct nt_view *parent)
{
	for (size_t count = nt_view_child_count(parent); count > 0; count--) {
		struct nt_view *previous = nt_view_child(parent, count - 1);
		if (nt_view_is_hidden(previous))
			continue;

		return nt_view_options(previous).margin.bottom > 0 ? 0 : 1;
	}

	return 0;
}

struct nt_view *cli_command_add(struct nt_builder *builder, struct nt_view *parent,
				const char *text, int styled)
{
	const struct nt_view_options options = {
		.width = NT_FILL,
		.max_width = NT_PERCENT(100),
		.margin = {.top = cli_command_top_margin(parent), .bottom = 1}};
	struct nt_view *group = nt_column_add(builder, parent, 1, &options);
	if (!group)
		return NULL;

	struct nt_view_options heading = {0};
	if (styled)
		heading.style.foreground = (struct nt_color){NT_COLOR_INDEXED, CLI_COMMAND_INDEX};

	struct nt_view *label = nt_label_add(builder, group, text, &heading);
	if (label)
		nt_builder_fail(builder, nt_label_set_wrap_mode(label, NT_TEXT_WRAP_NONE));

	return group;
}
