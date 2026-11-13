#include <morph/tui/widgets/tree.h>
#include "nb/err.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
#include "nb/cleanup.h"
#include <stdlib.h>
#include <string.h>

const char *morph_tui_widget_tree_branch(int is_last, int continuation)
{
	return continuation ? (is_last ? "  " : "│ ") : (is_last ? "└ " : "├ ");
}

int morph_tui_widget_tree_prefix(nb_buf *output, const int *ancestors_last, size_t depth,
				 int is_last, int continuation, size_t indent)
{
	if (!output || (depth && !ancestors_last))
		return NB_ERR_INVAL;
	for (size_t i = 0; i < indent; i++)
		nb_buf_append_char(output, ' ');
	for (size_t i = 0; i < depth; i++)
		nb_buf_append_cstr(output, ancestors_last[i] ? "  " : "│ ");
	nb_buf_append_cstr(output, morph_tui_widget_tree_branch(is_last, continuation));
	return nb_buf_status(output);
}

int morph_tui_widget_tree_init(struct morph_tui_widget_tree *tree)
{
	if (!tree)
		return NB_ERR_INVAL;
	memset(tree, 0, sizeof(*tree));
	tree->first_root = MORPH_TUI_WIDGET_TREE_ROOT;
	tree->last_root = MORPH_TUI_WIDGET_TREE_ROOT;
	int rc = (nb_vec_init(&tree->nodes, sizeof(struct morph_tui_widget_tree_node)),
		  nb_vec_reserve(&tree->nodes, 16) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = (nb_vec_init(&tree->visible, sizeof(size_t)),
		      nb_vec_reserve(&tree->visible, 16) ? 0 : NB_ERR_NOMEM);
	if (rc) {
		morph_tui_widget_tree_cleanup(tree);
		return tree->error = rc;
	}
	return 0;
}

void morph_tui_widget_tree_cleanup(struct morph_tui_widget_tree *tree)
{
	if (!tree)
		return;
	for (size_t i = 0; i < tree->nodes.len; i++) {
		struct morph_tui_widget_tree_node *node = nb_vec_at(&tree->nodes, i);

		free(node->label);
		free(node->description);
	}
	nb_vec_cleanup(&tree->nodes);
	nb_vec_cleanup(&tree->visible);
	memset(tree, 0, sizeof(*tree));
}

static char *widget_tree_copy_text(const char *text)
{
	return morph_terminal_sanitize_dup(text ? text : "", text ? strlen(text) : 0,
					   MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
}

size_t morph_tui_widget_tree_add(struct morph_tui_widget_tree *tree, size_t parent,
				 const char *label, const char *description)
{
	if (!tree || tree->error)
		return MORPH_TUI_WIDGET_TREE_ROOT;
	if (!tree->nodes.elem_size ||
	    (parent != MORPH_TUI_WIDGET_TREE_ROOT && parent >= tree->nodes.len)) {
		tree->error = (NB_ERR_INVAL);
		return MORPH_TUI_WIDGET_TREE_ROOT;
	}
	char *name NB_CLEANUP(nb_charp_cleanup) = widget_tree_copy_text(label);
	char *detail NB_CLEANUP(nb_charp_cleanup) = widget_tree_copy_text(description);
	struct morph_tui_widget_tree_node *node = name && detail ? nb_vec_push(&tree->nodes) : NULL;
	if (!node) {
		tree->error = (NB_ERR_NOMEM);
		return MORPH_TUI_WIDGET_TREE_ROOT;
	}
	size_t index = tree->nodes.len - 1;
	*node = (struct morph_tui_widget_tree_node){parent,
						    MORPH_TUI_WIDGET_TREE_ROOT,
						    MORPH_TUI_WIDGET_TREE_ROOT,
						    MORPH_TUI_WIDGET_TREE_ROOT,
						    nb_charp_steal(&name),
						    nb_charp_steal(&detail),
						    1};
	struct morph_tui_widget_tree_node *owner =
		parent == MORPH_TUI_WIDGET_TREE_ROOT ? NULL : nb_vec_at(&tree->nodes, parent);
	size_t last = owner ? owner->last_child : tree->last_root;
	if (last != MORPH_TUI_WIDGET_TREE_ROOT) {
		struct morph_tui_widget_tree_node *sibling = nb_vec_at(&tree->nodes, last);
		sibling->next_sibling = index;
	} else if (owner) {
		owner->first_child = index;
	} else {
		tree->first_root = index;
	}
	if (owner)
		owner->last_child = index;
	else
		tree->last_root = index;
	return index;
}

static struct morph_tui_widget_tree_node *
widget_tree_node_at(const struct morph_tui_widget_tree *tree, size_t index)
{
	return nb_vec_at(&tree->nodes, index);
}

static int widget_tree_has_children(const struct morph_tui_widget_tree *tree, size_t index)
{
	return widget_tree_node_at(tree, index)->first_child != MORPH_TUI_WIDGET_TREE_ROOT;
}

static int widget_tree_visit(struct morph_tui_widget_tree *tree)
{
	size_t index = tree->first_root;
	while (index != MORPH_TUI_WIDGET_TREE_ROOT) {
		size_t *entry = nb_vec_push(&tree->visible);
		if (!entry)
			return NB_ERR_NOMEM;
		*entry = index;
		struct morph_tui_widget_tree_node *node = widget_tree_node_at(tree, index);
		if (node->expanded && node->first_child != MORPH_TUI_WIDGET_TREE_ROOT) {
			index = node->first_child;
			continue;
		}
		while (node->next_sibling == MORPH_TUI_WIDGET_TREE_ROOT &&
		       node->parent != MORPH_TUI_WIDGET_TREE_ROOT)
			node = widget_tree_node_at(tree, node->parent);
		index = node->next_sibling;
	}
	return 0;
}

static int widget_tree_refresh(struct morph_tui_widget_tree *tree, size_t page)
{
	if (!tree->nodes.elem_size)
		return NB_ERR_INVAL;
	if (tree->error)
		return tree->error;
	nb_vec_clear(&tree->visible);
	int rc = widget_tree_visit(tree);
	if (rc)
		return tree->error = rc;
	morph_tui_widget_list_state_layout(&tree->list, tree->visible.len, page);
	return 0;
}

size_t morph_tui_widget_tree_selected(const struct morph_tui_widget_tree *tree)
{
	if (!tree)
		return MORPH_TUI_WIDGET_TREE_ROOT;
	size_t *entry = nb_vec_at(&tree->visible, tree->list.index);
	return entry ? *entry : MORPH_TUI_WIDGET_TREE_ROOT;
}

int morph_tui_widget_tree_update(struct morph_tui_widget_tree *tree, int key, size_t page)
{
	if (!tree)
		return MORPH_TUI_WIDGET_UNHANDLED;
	if (key == MORPH_TUI_KEY_PASTE_BEGIN || key == MORPH_TUI_KEY_PASTE_END) {
		tree->pasting = key == MORPH_TUI_KEY_PASTE_BEGIN;
		return MORPH_TUI_WIDGET_UNHANDLED;
	}
	if (tree->pasting || widget_tree_refresh(tree, page))
		return MORPH_TUI_WIDGET_UNHANDLED;
	if (key == 27 || key == 3)
		return MORPH_TUI_WIDGET_CANCEL;
	size_t index = morph_tui_widget_tree_selected(tree);
	if (index == MORPH_TUI_WIDGET_TREE_ROOT)
		return MORPH_TUI_WIDGET_UNHANDLED;
	struct morph_tui_widget_tree_node *node = widget_tree_node_at(tree, index);
	if (key == '\r' || key == '\n')
		return MORPH_TUI_WIDGET_SUBMIT;
	size_t old_index = tree->list.index;
	int old_expanded = node->expanded;
	if (key == MORPH_TUI_KEY_LEFT || key == MORPH_TUI_KEY_RIGHT) {
		if (key == MORPH_TUI_KEY_RIGHT && widget_tree_has_children(tree, index)) {
			if (node->expanded)
				tree->list.index++;
			else
				node->expanded = 1;
		} else if (key == MORPH_TUI_KEY_LEFT) {
			if (widget_tree_has_children(tree, index) && node->expanded)
				node->expanded = 0;
			else if (node->parent != MORPH_TUI_WIDGET_TREE_ROOT) {
				for (size_t i = 0; i < tree->visible.len; i++)
					if (*(size_t *)nb_vec_at(&tree->visible, i) == node->parent)
						tree->list.index = i;
			}
		}
		(void)widget_tree_refresh(tree, page);
		return old_index != tree->list.index || old_expanded != node->expanded
			       ? MORPH_TUI_WIDGET_CHANGED
			       : MORPH_TUI_WIDGET_UNHANDLED;
	}
	return morph_tui_widget_list_state_update(&tree->list, key);
}

static int widget_tree_last_sibling(const struct morph_tui_widget_tree *tree, size_t index)
{
	return widget_tree_node_at(tree, index)->next_sibling == MORPH_TUI_WIDGET_TREE_ROOT;
}

int morph_tui_widget_tree_render(struct morph_tui_widget_tree *tree, nb_buf *output, int columns,
				 size_t first, size_t rows)
{
	if (!tree || !output || columns < 1)
		return NB_ERR_INVAL;
	if (nb_buf_status(output))
		return nb_buf_status(output);
	int rc = widget_tree_refresh(tree, rows);
	if (rc)
		return rc;
	for (size_t row = first; row < tree->visible.len && row - first < rows; row++) {
		size_t index = *(size_t *)nb_vec_at(&tree->visible, row);
		struct morph_tui_widget_tree_node *node = widget_tree_node_at(tree, index);
		nb_buf line NB_CLEANUP(nb_buf_cleanup) = {0};
		nb_vec path = {0};
		rc = (nb_buf_init(&line), nb_buf_reserve(&line, 128) ? 0 : NB_ERR_NOMEM);
		if (rc)
			return rc;
		rc = (nb_vec_init(&path, sizeof(int)), nb_vec_reserve(&path, 8) ? 0 : NB_ERR_NOMEM);
		if (rc) {
			nb_buf_cleanup(&line);
			return rc;
		}
		for (size_t p = node->parent; p != MORPH_TUI_WIDGET_TREE_ROOT;
		     p = widget_tree_node_at(tree, p)->parent) {
			int *entry = nb_vec_push(&path);
			if (!entry) {
				rc = NB_ERR_NOMEM;
				break;
			}
			*entry = widget_tree_last_sibling(tree, p);
		}
		for (size_t i = 0; i < path.len / 2; i++) {
			int *a = nb_vec_at(&path, i);
			int *b = nb_vec_at(&path, path.len - 1 - i);
			int swap = *a;
			*a = *b;
			*b = swap;
		}
		if (!rc) {
			morph_tui_widget_tree_prefix(&line, ((void *)path.data), path.len,
						     widget_tree_last_sibling(tree, index), 0, 2);
			if (widget_tree_has_children(tree, index))
				nb_buf_append_cstr(&line, node->expanded ? "▾ " : "▸ ");
			if (node->description[0]) {
				int width = columns / 3;
				if (width > 36)
					width = 36;
				if (width < 1)
					width = 1;
				char *label = malloc(strlen(node->label) + 4);
				if (!label)
					rc = NB_ERR_NOMEM;
				else {
					morph_terminal_copy_ellipsized_display_width(
						label, strlen(node->label) + 4, node->label,
						(size_t)width, 0);
					int padding = width - (int)nb_utf8_width_cstr(label);
					nb_buf_printf(&line, "%s%*s  %s", label, padding, "",
						      node->description);
					free(label);
				}
			} else {
				nb_buf_append_cstr(&line, node->label);
			}
			char *clipped = malloc(line.len + 4);
			if (!clipped)
				rc = NB_ERR_NOMEM;
			else {
				morph_terminal_copy_ellipsized_display_width(clipped, line.len + 4,
									     nb_buf_cstr(&line),
									     (size_t)columns, 0);
				nb_buf_printf(output, "%s\n", clipped);
				free(clipped);
			}
		}
		if (!rc)
			rc = nb_buf_status(&line) ? nb_buf_status(&line) : nb_buf_status(output);
		nb_vec_cleanup(&path);
		nb_buf_cleanup(&line);
		if (rc)
			return rc;
	}
	return 0;
}

static int widget_tree_append_report_line(nb_buf *output, const char *line, int columns)
{
	if (morph_terminal_display_width_ansi(line) <= (size_t)columns)
		return (nb_buf_printf(output, "%s\n", line) ? 0 : NB_ERR_NOMEM);
	char *plain = morph_terminal_strip_ansi_dup(line, strlen(line), NULL);
	char *clipped = plain ? malloc(strlen(plain) + 4) : NULL;
	if (!plain || !clipped) {
		free(plain);
		free(clipped);
		return NB_ERR_NOMEM;
	}
	morph_terminal_copy_ellipsized_display_width(clipped, strlen(plain) + 4, plain,
						     (size_t)columns, 0);
	int rc = (nb_buf_printf(output, "%s\n", clipped) ? 0 : NB_ERR_NOMEM);
	free(clipped);
	free(plain);
	return rc;
}

int morph_tui_widget_tree_row(nb_buf *output, const char *ancestor, int is_last, const char *marker,
			      const char *label, const char *description, int label_width,
			      int columns, const char *style, const char *reset)
{
	if (!output || !ancestor || !marker || !label || !description || !style || !reset ||
	    columns < 1)
		return NB_ERR_INVAL;
	nb_buf line NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&line), nb_buf_reserve(&line, 128) ? 0 : NB_ERR_NOMEM);
	if (rc)
		return rc;
	char *safe = morph_terminal_sanitize_dup(label, strlen(label),
						 MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	char *detail = morph_terminal_sanitize_dup(description, strlen(description),
						   MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	char *clipped = safe ? malloc(strlen(safe) + 4) : NULL;
	char *clipped_detail = detail ? malloc(strlen(detail) + 4) : NULL;
	if (!safe || !detail || !clipped || !clipped_detail) {
		rc = NB_ERR_NOMEM;
	} else {
		int prefix = 4 + (int)morph_terminal_display_width_ansi(ancestor);
		int mark = marker[0] ? (int)morph_terminal_display_width_ansi(marker) + 1 : 0;
		int available = columns - prefix - mark;
		int natural = (int)nb_utf8_width_cstr(safe);
		if (label_width < natural)
			label_width = natural;
		if (label_width < 1 || label_width > available)
			label_width = available;
		if (label_width < 0)
			label_width = 0;
		morph_terminal_copy_ellipsized_display_width(clipped, strlen(safe) + 4, safe,
							     (size_t)label_width, 0);
		int used = (int)nb_utf8_width_cstr(clipped);
		nb_buf_printf(&line, "  %s%s%s%s%s%s%s", ancestor,
			      morph_tui_widget_tree_branch(is_last, 0), marker,
			      marker[0] ? " " : "", style, clipped, reset);
		int inline_detail = detail[0] && columns - prefix - mark - label_width - 2 >= 18;
		if (inline_detail) {
			morph_terminal_copy_ellipsized_display_width(
				clipped_detail, strlen(detail) + 4, detail,
				(size_t)(columns - prefix - mark - label_width - 2), 0);
			nb_buf_printf(&line, "%*s  %s", label_width - used, "", clipped_detail);
		}
		rc = widget_tree_append_report_line(output, nb_buf_cstr(&line), columns);
		if (detail[0] && !inline_detail) {
			int width = columns - prefix - 2;
			if (width < 0)
				width = 0;
			morph_terminal_copy_ellipsized_display_width(
				clipped_detail, strlen(detail) + 4, detail, (size_t)width, 0);
			nb_buf_clear(&line);
			nb_buf_printf(&line, "  %s%s  %s", ancestor,
				      morph_tui_widget_tree_branch(is_last, 1), clipped_detail);
			if (!rc)
				rc = widget_tree_append_report_line(output, nb_buf_cstr(&line),
								    columns);
		}
		rc = rc ? rc
			: (nb_buf_status(&line) ? nb_buf_status(&line) : nb_buf_status(output));
	}
	free(clipped_detail);
	free(clipped);
	free(detail);
	free(safe);
	nb_buf_cleanup(&line);
	return rc;
}

int morph_tui_widget_tree_draw(struct morph_tui_widget_tree *tree, nb_buf *commands,
			       struct morph_tui_terminal_rect area, const char *normal,
			       const char *selected)
{
	if (!tree || !commands || !normal || !selected)
		return NB_ERR_INVAL;
	if (area.width <= 0 || area.height <= 0)
		return 0;
	int rc = widget_tree_refresh(tree, (size_t)area.height);
	if (rc)
		return rc;
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	rc = (nb_buf_init(&text), nb_buf_reserve(&text, 256) ? 0 : NB_ERR_NOMEM);
	if (rc)
		return rc;
	rc = morph_tui_widget_tree_render(tree, &text, area.width, tree->list.first,
					  (size_t)area.height);
	char *line = text.ptr;
	for (int row = 0; !rc && row < area.height; row++) {
		char *end = line ? strchr(line, '\n') : NULL;
		if (end)
			*end = '\0';
		rc = morph_tui_draw_region_cell(
			commands, area, row, 0, area.width, line ? line : "",
			tree->list.first + (size_t)row == tree->list.index ? selected : normal);
		line = end && end[1] ? end + 1 : NULL;
	}
	nb_buf_cleanup(&text);
	return rc;
}
