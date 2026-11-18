#include "sapi/cli/presentation/panel.h"

static int cli_panel_condition(void *data, struct nt_value *value)
{
	return nt_value_set_bool(value, nt_component_state_get_bool(data));
}

static int cli_panel_inverse(void *data, struct nt_value *value)
{
	return nt_value_set_bool(value, !nt_component_state_get_bool(data));
}

void cli_panel_show_when(struct nt_builder *builder, struct nt_view *view,
			 struct nt_component_state *condition)
{
	nt_builder_fail(builder,
			nt_view_bind_compute(view, NT_PROP_HIDDEN, cli_panel_inverse, condition));
}

void cli_panel_hide_when(struct nt_builder *builder, struct nt_view *view,
			 struct nt_component_state *condition)
{
	nt_builder_fail(builder,
			nt_view_bind_compute(view, NT_PROP_HIDDEN, cli_panel_condition, condition));
}
