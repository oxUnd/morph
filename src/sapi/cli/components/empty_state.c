#include "sapi/cli/components/empty_state.h"

#include "sapi/cli/presentation/style.h"
#include <nb/err.h>
#include <nb/utf8.h>
#include <nothing/tui/controls/button.h>
#include <nothing/tui/theme.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/stack_view.h>
#include <limits.h>

struct cli_empty_state {
	struct nt_stack_view base;
	int (*activate)(void *data, size_t index);
	void *data;
};

static int cli_empty_state_activate(void *data, const struct nt_view_event *event)
{
	struct cli_empty_state *state = data;
	int rc = state->activate ? state->activate(state->data, (size_t)event->tag) : 0;
	return rc < 0 ? rc : NT_VIEW_EVENT_STOP;
}

struct nt_view *cli_empty_state_add(struct nt_builder *builder, struct nt_view *parent,
	const char *title, const char *description,
	const struct cli_empty_state_action *actions, size_t count, int styled,
	int (*activate)(void *data, size_t index), void *data,
	const struct nt_view_options *options)
{
	if (!title || !description || (count && !actions) || count > INT_MAX) {
		nt_builder_fail(builder, NB_ERR_INVAL);
		return NULL;
	}

	int action_width = 0;
	for (size_t i = 0; i < count; i++) {
		if (!actions[i].label || !actions[i].description) {
			nt_builder_fail(builder, NB_ERR_INVAL);
			return NULL;
		}

		size_t width = nb_utf8_width_cstr(actions[i].label);
		if (width > INT_MAX - 2u) {
			nt_builder_fail(builder, NB_ERR_RANGE);
			return NULL;
		}

		if ((int)width + 2 > action_width)
			action_width = (int)width + 2;
	}

	static const struct nt_view_class type = {
		.superclass = &nt_stack_view_class,
		.instance_size = sizeof(struct cli_empty_state),
		.kind = "cli_empty_state",
	};
	struct cli_empty_state *state = (struct cli_empty_state *)nt_view_add(
		builder, parent, &type, options);
	if (!state)
		return NULL;

	state->activate = activate;
	state->data = data;
	struct nt_view *view = &state->base.base;
	struct nt_text_style muted = styled
		? nt_theme_style(nt_view_theme(view), NT_THEME_MUTED, NT_VISUAL_NORMAL)
		: (struct nt_text_style){0};
	nt_label_add(builder, view, title,
		&(struct nt_view_options){.style = {.attributes = NT_STYLE_BOLD}});
	if (*description)
		nt_label_add(builder, view, description,
			&(struct nt_view_options){.margin = {.bottom = 1}, .style = muted});

	for (size_t i = 0; i < count; i++) {
		struct nt_view *row = nt_row_add(builder, view, 1, NULL);
		struct nt_text_style command = {0};
		if (styled)
			command.foreground = (struct nt_color){NT_COLOR_INDEXED, CLI_COMMAND_INDEX};

		struct nt_view *button = nt_button_add(builder, row, actions[i].label, (int)i,
			&(struct nt_view_options){.width = action_width, .style = command});
		if (button) {
			nt_builder_fail(builder, nt_view_set_focusable(button, 0));
			if (!nt_view_on(button, NT_VIEW_EVENT_ACTIVATE, cli_empty_state_activate, state))
				nt_builder_fail(builder, NB_ERR_NOMEM);
		}

		nt_label_add(builder, row, actions[i].description,
			&(struct nt_view_options){.width = NT_FILL, .style = muted});
	}

	return view;
}
