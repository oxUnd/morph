#ifndef MORPH_CLI_COMPONENTS_SKILL_TREE_H
#define MORPH_CLI_COMPONENTS_SKILL_TREE_H
#include <nb/buf.h>
#include <nothing/tui/view.h>

struct cli_skill_list_row;
struct nt_view *cli_skill_tree_add(struct nt_builder *, struct nt_view *parent,
				   const struct cli_skill_list_row *, size_t count);
int cli_skill_tree_render(nb_buf *out, int width, const struct cli_skill_list_row *, size_t count);

#endif
