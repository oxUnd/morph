#ifndef MORPH_TUI_WIDGET_TREE_H
#define MORPH_TUI_WIDGET_TREE_H

#include <morph/tui/layout.h>
#include <morph/tui/widgets/list.h>
#include "nb/vec.h"
#include "nb/cleanup.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MORPH_TUI_WIDGET_TREE_ROOT SIZE_MAX

/* Text is copied and sanitized. Node indices survive array growth. */
struct morph_tui_widget_tree_node {
	size_t parent;
	size_t first_child;
	size_t last_child;
	size_t next_sibling;
	char *label;
	char *description;
	int expanded;
};

struct morph_tui_widget_tree {
	nb_vec nodes;
	nb_vec visible;
	struct morph_tui_widget_list_state list;
	size_t first_root;
	size_t last_root;
	int error;
	int pasting;
};

int morph_tui_widget_tree_init(struct morph_tui_widget_tree *tree);
void morph_tui_widget_tree_cleanup(struct morph_tui_widget_tree *tree);
size_t morph_tui_widget_tree_add(struct morph_tui_widget_tree *tree, size_t parent,
				 const char *label, const char *description);
int morph_tui_widget_tree_update(struct morph_tui_widget_tree *tree, int key, size_t page);
size_t morph_tui_widget_tree_selected(const struct morph_tui_widget_tree *tree);
/* Render visible nodes as newline-separated rows, bounded by display columns. */
int morph_tui_widget_tree_render(struct morph_tui_widget_tree *tree, nb_buf *output, int columns,
				 size_t first, size_t rows);
int morph_tui_widget_tree_draw(struct morph_tui_widget_tree *tree, nb_buf *commands,
			       struct morph_tui_terminal_rect area, const char *normal,
			       const char *selected);
/* Shared geometry for incremental reports that cannot retain a full model. */
/* Marker and styles are trusted terminal formatting; text is sanitized. */
int morph_tui_widget_tree_row(nb_buf *output, const char *ancestor, int is_last, const char *marker,
			      const char *label, const char *description, int label_width,
			      int columns, const char *style, const char *reset);
const char *morph_tui_widget_tree_branch(int is_last, int continuation);
int morph_tui_widget_tree_prefix(nb_buf *output, const int *ancestors_last, size_t depth,
				 int is_last, int continuation, size_t indent);

#ifdef __cplusplus
}
#endif
#endif
