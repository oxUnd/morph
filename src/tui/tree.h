#ifndef MORPH_TUI_TREE_H
#define MORPH_TUI_TREE_H

#include "tui/components.h"
#include "util/arena.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TUI_TREE_ROOT SIZE_MAX

/* Text is copied and sanitized. Node indices survive array growth. */
struct tui_tree_node {
	size_t parent;
	size_t first_child;
	size_t last_child;
	size_t next_sibling;
	const char *label;
	const char *description;
	int expanded;
};

struct tui_tree {
	struct arena *arena;
	morph_array_t nodes;
	morph_array_t visible;
	struct tui_list list;
	size_t first_root;
	size_t last_root;
	int error;
	int pasting;
};

int tui_tree_init(struct tui_tree *tree);
void tui_tree_cleanup(struct tui_tree *tree);
size_t tui_tree_add(struct tui_tree *tree, size_t parent,
		    const char *label, const char *description);
int tui_tree_update(struct tui_tree *tree, int key, size_t page);
size_t tui_tree_selected(const struct tui_tree *tree);
/* Render visible nodes as newline-separated rows, bounded by display columns. */
int tui_tree_render(struct tui_tree *tree, morph_buf_t *output, int columns,
		    size_t first, size_t rows);
int tui_tree_draw(struct tui_tree *tree, morph_buf_t *commands,
		  struct tui_rect area, const char *normal, const char *selected);
/* Shared geometry for incremental reports that cannot retain a full model. */
/* Marker and styles are trusted terminal formatting; text is sanitized. */
int tui_tree_row(morph_buf_t *output, const char *ancestor, int is_last,
		 const char *marker, const char *label, const char *description,
		 int label_width, int columns, const char *style, const char *reset);
const char *tui_tree_branch(int is_last, int continuation);
int tui_tree_prefix(morph_buf_t *output, const int *ancestors_last,
		    size_t depth, int is_last, int continuation, size_t indent);

#ifdef __cplusplus
}
#endif
#endif
