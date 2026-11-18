#ifndef MORPH_CLI_COMPONENTS_VISIBILITY_H
#define MORPH_CLI_COMPONENTS_VISIBILITY_H

#include <nothing/tui/component_state.h>
#include <nothing/tui/view_controller.h>

/* Bind the same condition to complementary parts of a page. */
void cli_bind_visible(struct nt_builder *builder, struct nt_view *view,
		      struct nt_component_state *condition);
void cli_bind_hidden(struct nt_builder *builder, struct nt_view *view,
		     struct nt_component_state *condition);

#endif
