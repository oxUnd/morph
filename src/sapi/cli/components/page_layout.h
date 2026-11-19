#ifndef MORPH_CLI_COMPONENTS_PAGE_LAYOUT_H
#define MORPH_CLI_COMPONENTS_PAGE_LAYOUT_H

#include <nothing/tui/view.h>

struct cli_page_layout {
	struct nt_view *root, *body, *footer, *subpage;
};

/* The page owns navigation state. Back returns to its top-level content. */
typedef int (*cli_page_layout_back_fn)(void *data);

void cli_page_layout_build(struct nt_builder *builder, struct cli_page_layout *layout,
			   const char *name, const struct nt_view_class *body_class,
			   cli_page_layout_back_fn back, void *data);
void cli_page_layout_bind_subpage(struct nt_builder *builder, struct cli_page_layout *layout,
				  nt_compute_fn compute, void *data);
int cli_page_layout_set_subpage(struct cli_page_layout *layout, const char *name);

#endif
