#ifndef MORPH_CLI_PRESENTATION_PANEL_STYLE_H
#define MORPH_CLI_PRESENTATION_PANEL_STYLE_H

#include <nothing/tui/view.h>

static inline struct nt_view_options cli_panel_heading(int height)
{
	return (struct nt_view_options){.height = height, .style = {.attributes = NT_STYLE_BOLD}};
}

static inline struct nt_view_options cli_panel_hint(int height)
{
	return (struct nt_view_options){.height = height, .style = {.attributes = NT_STYLE_DIM}};
}

#endif
