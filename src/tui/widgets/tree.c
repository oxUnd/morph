#include <morph/tui/tree.h>
#include "util/utf8.h"
#include <stdlib.h>
#include <string.h>

const char *morph_tui_tree_branch(int is_last, int continuation)
{
	return continuation ? (is_last ? "  " : "│ ") : (is_last ? "└ " : "├ ");
}

int morph_tui_tree_prefix(morph_buf_t *output, const int *ancestors_last,
		    size_t depth, int is_last, int continuation, size_t indent)
{
	if (!output || (depth && !ancestors_last))
		return -EINVAL;
	for (size_t i = 0; i < indent; i++)
		morph_buf_putc(output, ' ');
	for (size_t i = 0; i < depth; i++)
		morph_buf_puts(output, ancestors_last[i] ? "  " : "│ ");
	morph_buf_puts(output, morph_tui_tree_branch(is_last, continuation));
	return output->failed;
}

int morph_tui_tree_init(struct morph_tui_tree *tree)
{
	if (!tree)
		return -EINVAL;
	memset(tree, 0, sizeof(*tree));
	tree->first_root = MORPH_TUI_TREE_ROOT;
	tree->last_root = MORPH_TUI_TREE_ROOT;
	tree->arena = arena_create(4096);
	if (!tree->arena)
		return tree->error = -ENOMEM;
	int rc = morph_array_init(&tree->nodes, 16, sizeof(struct morph_tui_tree_node));
	if (!rc)
		rc = morph_array_init(&tree->visible, 16, sizeof(size_t));
	if (rc) {
		morph_tui_tree_cleanup(tree);
		return tree->error = rc;
	}
	return 0;
}

void morph_tui_tree_cleanup(struct morph_tui_tree *tree)
{
	if (!tree)
		return;
	morph_array_cleanup(&tree->nodes);
	morph_array_cleanup(&tree->visible);
	arena_destroy(tree->arena);
	memset(tree, 0, sizeof(*tree));
}

static const char *copy_text(struct morph_tui_tree *tree, const char *text)
{
	char *safe = utf8_terminal_sanitize_dup(text ? text : "",
		text ? strlen(text) : 0, UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return NULL;
	const char *copy = arena_strdup(tree->arena, safe);
	free(safe);
	return copy;
}

size_t morph_tui_tree_add(struct morph_tui_tree *tree, size_t parent,
		    const char *label, const char *description)
{
	if (!tree || tree->error)
		return MORPH_TUI_TREE_ROOT;
	if (!tree->arena || (parent != MORPH_TUI_TREE_ROOT && parent >= tree->nodes.nelts)) {
		tree->error = (-EINVAL);
		return MORPH_TUI_TREE_ROOT;
	}
	const char *name = copy_text(tree, label);
	const char *detail = copy_text(tree, description);
	struct morph_tui_tree_node *node = name && detail ? morph_array_push(&tree->nodes) : NULL;
	if (!node) {
		tree->error = (-ENOMEM);
		return MORPH_TUI_TREE_ROOT;
	}
	size_t index = tree->nodes.nelts - 1;
	*node = (struct morph_tui_tree_node){parent, MORPH_TUI_TREE_ROOT, MORPH_TUI_TREE_ROOT,
		MORPH_TUI_TREE_ROOT, name, detail, 1};
	struct morph_tui_tree_node *owner = parent == MORPH_TUI_TREE_ROOT ? NULL :
		morph_array_get(&tree->nodes, parent);
	size_t last = owner ? owner->last_child : tree->last_root;
	if (last != MORPH_TUI_TREE_ROOT) {
		struct morph_tui_tree_node *sibling = morph_array_get(&tree->nodes, last);
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

static struct morph_tui_tree_node *node_at(const struct morph_tui_tree *tree, size_t index)
{
	return morph_array_get(&tree->nodes, index);
}

static int has_children(const struct morph_tui_tree *tree, size_t index)
{
	return node_at(tree, index)->first_child != MORPH_TUI_TREE_ROOT;
}

static int visit(struct morph_tui_tree *tree)
{
	size_t index = tree->first_root;
	while (index != MORPH_TUI_TREE_ROOT) {
		size_t *entry = morph_array_push(&tree->visible);
		if (!entry)
			return -ENOMEM;
		*entry = index;
		struct morph_tui_tree_node *node = node_at(tree, index);
		if (node->expanded && node->first_child != MORPH_TUI_TREE_ROOT) {
			index = node->first_child;
			continue;
		}
		while (node->next_sibling == MORPH_TUI_TREE_ROOT &&
		       node->parent != MORPH_TUI_TREE_ROOT)
			node = node_at(tree, node->parent);
		index = node->next_sibling;
	}
	return 0;
}

static int refresh(struct morph_tui_tree *tree, size_t page)
{
	if (!tree->arena)
		return -EINVAL;
	if (tree->error)
		return tree->error;
	morph_array_clear(&tree->visible);
	int rc = visit(tree);
	if (rc)
		return tree->error = rc;
	morph_tui_list_layout(&tree->list, tree->visible.nelts, page);
	return 0;
}

size_t morph_tui_tree_selected(const struct morph_tui_tree *tree)
{
	if (!tree)
		return MORPH_TUI_TREE_ROOT;
	size_t *entry = morph_array_get(&tree->visible, tree->list.index);
	return entry ? *entry : MORPH_TUI_TREE_ROOT;
}

int morph_tui_tree_update(struct morph_tui_tree *tree, int key, size_t page)
{
	if (!tree)
		return MORPH_TUI_UNHANDLED;
	if (key == MORPH_TUI_KEY_PASTE_BEGIN || key == MORPH_TUI_KEY_PASTE_END) {
		tree->pasting = key == MORPH_TUI_KEY_PASTE_BEGIN;
		return MORPH_TUI_UNHANDLED;
	}
	if (tree->pasting || refresh(tree, page))
		return MORPH_TUI_UNHANDLED;
	if (key == 27 || key == 3)
		return MORPH_TUI_CANCEL;
	size_t index = morph_tui_tree_selected(tree);
	if (index == MORPH_TUI_TREE_ROOT)
		return MORPH_TUI_UNHANDLED;
	struct morph_tui_tree_node *node = node_at(tree, index);
	if (key == '\r' || key == '\n')
		return MORPH_TUI_SUBMIT;
	size_t old_index = tree->list.index;
	int old_expanded = node->expanded;
	if (key == MORPH_TUI_KEY_LEFT || key == MORPH_TUI_KEY_RIGHT) {
		if (key == MORPH_TUI_KEY_RIGHT && has_children(tree, index)) {
			if (node->expanded)
				tree->list.index++;
			else
				node->expanded = 1;
		} else if (key == MORPH_TUI_KEY_LEFT) {
			if (has_children(tree, index) && node->expanded)
				node->expanded = 0;
			else if (node->parent != MORPH_TUI_TREE_ROOT) {
				for (size_t i = 0; i < tree->visible.nelts; i++)
					if (*(size_t *)morph_array_get(&tree->visible, i) ==
					    node->parent)
						tree->list.index = i;
			}
		}
		(void)refresh(tree, page);
		return old_index != tree->list.index || old_expanded != node->expanded ?
			MORPH_TUI_CHANGED : MORPH_TUI_UNHANDLED;
	}
	return morph_tui_list_update(&tree->list, key);
}

static int last_sibling(const struct morph_tui_tree *tree, size_t index)
{
	return node_at(tree, index)->next_sibling == MORPH_TUI_TREE_ROOT;
}

int morph_tui_tree_render(struct morph_tui_tree *tree, morph_buf_t *output, int columns,
		    size_t first, size_t rows)
{
	if (!tree || !output || columns < 1)
		return -EINVAL;
	if (output->failed)
		return output->failed;
	int rc = refresh(tree, rows);
	if (rc)
		return rc;
	for (size_t row = first; row < tree->visible.nelts && row - first < rows; row++) {
		size_t index = *(size_t *)morph_array_get(&tree->visible, row);
		struct morph_tui_tree_node *node = node_at(tree, index);
		morph_buf_t line = {0};
		morph_array_t path = {0};
		rc = morph_buf_init(&line, 128);
		if (rc)
			return rc;
		rc = morph_array_init(&path, 8, sizeof(int));
		if (rc) {
			morph_buf_cleanup(&line);
			return rc;
		}
		for (size_t p = node->parent; p != MORPH_TUI_TREE_ROOT;
		     p = node_at(tree, p)->parent) {
			int *entry = morph_array_push(&path);
			if (!entry) {
				rc = -ENOMEM;
				break;
			}
			*entry = last_sibling(tree, p);
		}
		for (size_t i = 0; i < path.nelts / 2; i++) {
			int *a = morph_array_get(&path, i);
			int *b = morph_array_get(&path, path.nelts - 1 - i);
			int swap = *a;
			*a = *b;
			*b = swap;
		}
		if (!rc) {
			morph_tui_tree_prefix(&line, path.elts, path.nelts,
				last_sibling(tree, index), 0, 2);
			if (has_children(tree, index))
				morph_buf_puts(&line, node->expanded ? "▾ " : "▸ ");
			if (node->description[0]) {
				int width = columns / 3;
				if (width > 36)
					width = 36;
				if (width < 1)
					width = 1;
				char *label = malloc(strlen(node->label) + 4);
				if (!label)
					rc = -ENOMEM;
				else {
					utf8_copy_ellipsized_display_width(label,
						strlen(node->label) + 4, node->label,
						(size_t)width, 0);
					int padding = width - (int)utf8_display_width(label);
					morph_buf_printf(&line, "%s%*s  %s", label,
						padding, "", node->description);
					free(label);
				}
			} else {
				morph_buf_puts(&line, node->label);
			}
			char *clipped = malloc(line.len + 4);
			if (!clipped)
				rc = -ENOMEM;
			else {
				utf8_copy_ellipsized_display_width(clipped, line.len + 4,
					morph_buf_cstr(&line), (size_t)columns, 0);
				morph_buf_printf(output, "%s\n", clipped);
				free(clipped);
			}
		}
		if (!rc)
			rc = line.failed ? line.failed : output->failed;
		morph_array_cleanup(&path);
		morph_buf_cleanup(&line);
		if (rc)
			return rc;
	}
	return 0;
}

static int append_report_line(morph_buf_t *output, const char *line, int columns)
{
	if (utf8_display_width_ansi(line) <= (size_t)columns)
		return morph_buf_printf(output, "%s\n", line);
	char *plain = utf8_strip_ansi_dup(line, strlen(line), NULL);
	char *clipped = plain ? malloc(strlen(plain) + 4) : NULL;
	if (!plain || !clipped) {
		free(plain);
		free(clipped);
		return -ENOMEM;
	}
	utf8_copy_ellipsized_display_width(clipped, strlen(plain) + 4,
		plain, (size_t)columns, 0);
	int rc = morph_buf_printf(output, "%s\n", clipped);
	free(clipped);
	free(plain);
	return rc;
}

int morph_tui_tree_row(morph_buf_t *output, const char *ancestor, int is_last,
		 const char *marker, const char *label, const char *description,
		 int label_width, int columns, const char *style, const char *reset)
{
	if (!output || !ancestor || !marker || !label || !description ||
	    !style || !reset || columns < 1)
		return -EINVAL;
	morph_buf_t line = {0};
	int rc = morph_buf_init(&line, 128);
	if (rc)
		return rc;
	char *safe = utf8_terminal_sanitize_dup(label, strlen(label),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	char *detail = utf8_terminal_sanitize_dup(description, strlen(description),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	char *clipped = safe ? malloc(strlen(safe) + 4) : NULL;
	char *clipped_detail = detail ? malloc(strlen(detail) + 4) : NULL;
	if (!safe || !detail || !clipped || !clipped_detail) {
		rc = -ENOMEM;
	} else {
		int prefix = 4 + (int)utf8_display_width_ansi(ancestor);
		int mark = marker[0] ? (int)utf8_display_width_ansi(marker) + 1 : 0;
		int available = columns - prefix - mark;
		int natural = (int)utf8_display_width(safe);
		if (label_width < natural)
			label_width = natural;
		if (label_width < 1 || label_width > available)
			label_width = available;
		if (label_width < 0)
			label_width = 0;
		utf8_copy_ellipsized_display_width(clipped, strlen(safe) + 4,
			safe, (size_t)label_width, 0);
		int used = (int)utf8_display_width(clipped);
		morph_buf_printf(&line, "  %s%s%s%s%s%s%s", ancestor,
			morph_tui_tree_branch(is_last, 0), marker, marker[0] ? " " : "",
			style, clipped, reset);
		int inline_detail = detail[0] && columns - prefix - mark - label_width - 2 >= 18;
		if (inline_detail) {
			utf8_copy_ellipsized_display_width(clipped_detail, strlen(detail) + 4,
				detail, (size_t)(columns - prefix - mark - label_width - 2), 0);
			morph_buf_printf(&line, "%*s  %s", label_width - used, "", clipped_detail);
		}
		rc = append_report_line(output, morph_buf_cstr(&line), columns);
		if (detail[0] && !inline_detail) {
			int width = columns - prefix - 2;
			if (width < 0)
				width = 0;
			utf8_copy_ellipsized_display_width(clipped_detail, strlen(detail) + 4,
				detail, (size_t)width, 0);
			morph_buf_reset(&line);
			morph_buf_printf(&line, "  %s%s  %s", ancestor,
				morph_tui_tree_branch(is_last, 1), clipped_detail);
			if (!rc)
				rc = append_report_line(output, morph_buf_cstr(&line), columns);
		}
		rc = rc ? rc : (line.failed ? line.failed : output->failed);
	}
	free(clipped_detail);
	free(clipped);
	free(detail);
	free(safe);
	morph_buf_cleanup(&line);
	return rc;
}

int morph_tui_tree_draw(struct morph_tui_tree *tree, morph_buf_t *commands,
		  struct morph_tui_rect area, const char *normal, const char *selected)
{
	if (!tree || !commands || !normal || !selected)
		return -EINVAL;
	if (area.width <= 0 || area.height <= 0)
		return 0;
	int rc = refresh(tree, (size_t)area.height);
	if (rc)
		return rc;
	morph_buf_t text = {0};
	rc = morph_buf_init(&text, 256);
	if (rc)
		return rc;
	rc = morph_tui_tree_render(tree, &text, area.width, tree->list.first,
		(size_t)area.height);
	char *line = text.data;
	for (int row = 0; !rc && row < area.height; row++) {
		char *end = line ? strchr(line, '\n') : NULL;
		if (end)
			*end = '\0';
		rc = morph_tui_region_cell(commands, area, row, 0, area.width,
			line ? line : "", tree->list.first + (size_t)row ==
			tree->list.index ? selected : normal);
		line = end && end[1] ? end + 1 : NULL;
	}
	morph_buf_cleanup(&text);
	return rc;
}
