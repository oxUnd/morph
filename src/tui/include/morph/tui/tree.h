#ifndef MORPH_TUI_TREE_H
#define MORPH_TUI_TREE_H

#include <morph/tui/components.h>
#include "util/arena.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MORPH_TUI_TREE_ROOT SIZE_MAX

/* Text is copied and sanitized. Node indices survive array growth. */
struct morph_tui_tree_node {
	size_t parent;
	size_t first_child;
	size_t last_child;
	size_t next_sibling;
	const char *label;
	const char *description;
	int expanded;
};

struct morph_tui_tree {
	struct arena *arena;
	morph_array_t nodes;
	morph_array_t visible;
	struct morph_tui_list list;
	size_t first_root;
	size_t last_root;
	int error;
	int pasting;
};

int morph_tui_tree_init(struct morph_tui_tree *tree);
void morph_tui_tree_cleanup(struct morph_tui_tree *tree);
size_t morph_tui_tree_add(struct morph_tui_tree *tree, size_t parent,
		    const char *label, const char *description);
int morph_tui_tree_update(struct morph_tui_tree *tree, int key, size_t page);
size_t morph_tui_tree_selected(const struct morph_tui_tree *tree);
/* Render visible nodes as newline-separated rows, bounded by display columns. */
int morph_tui_tree_render(struct morph_tui_tree *tree, morph_buf_t *output, int columns,
		    size_t first, size_t rows);
int morph_tui_tree_draw(struct morph_tui_tree *tree, morph_buf_t *commands,
		  struct morph_tui_rect area, const char *normal, const char *selected);
/* Shared geometry for incremental reports that cannot retain a full model. */
/* Marker and styles are trusted terminal formatting; text is sanitized. */
int morph_tui_tree_row(morph_buf_t *output, const char *ancestor, int is_last,
		 const char *marker, const char *label, const char *description,
		 int label_width, int columns, const char *style, const char *reset);
const char *morph_tui_tree_branch(int is_last, int continuation);
int morph_tui_tree_prefix(morph_buf_t *output, const int *ancestors_last,
		    size_t depth, int is_last, int continuation, size_t indent);

#ifdef __cplusplus
}
#endif
#endif
