#ifndef MORPH_CLI_PRESENTATION_PANEL_H
#define MORPH_CLI_PRESENTATION_PANEL_H

#include <nothing/tui/component_state.h>
#include <nothing/tui/view_controller.h>

static inline struct nt_view_options cli_panel_heading(int height)
{
	return (struct nt_view_options){.height = height, .style = {.attributes = NT_STYLE_BOLD}};
}

static inline struct nt_view_options cli_panel_hint(int height)
{
	return (struct nt_view_options){.height = height, .style = {.attributes = NT_STYLE_DIM}};
}

void cli_panel_show_when(struct nt_builder *builder, struct nt_view *view,
			 struct nt_component_state *condition);
void cli_panel_hide_when(struct nt_builder *builder, struct nt_view *view,
			 struct nt_component_state *condition);

#endif
