#include "sapi/cli/component/skill_tree.h"
#include "sapi/cli/presentation/command_output.h"
#include "sapi/cli/presentation/style.h"
#include <morph/tui/terminal/report.h>
#include <nothing/tui/controls/tree_view.h>
#include <nothing/tui/terminal/text.h>
#include <nb/cleanup.h>
#include <nb/utf8.h>
#include <stdlib.h>
#include <string.h>

static int cli_skill_tree_row(const struct nt_tree_node *node, int width, nb_buf *out)
{
	const char *branch = morph_terminal_tree_branch(node->next_sibling == SIZE_MAX, 0);
	int label_width = width / 3;
	if (label_width < 1)
		label_width = 1;
	if (label_width > 36)
		label_width = 36;
	nb_buf row NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!node->description || !*node->description) {
		nb_buf_printf(&row, "  %s%s", branch, node->label);
	} else {
		size_t capacity = strlen(node->label) + 4;
		char *label NB_CLEANUP(nb_charp_cleanup) = malloc(capacity);
		if (!label)
			return NB_ERR_NOMEM;
		nt_terminal_copy_ellipsized_display_width(label, capacity, node->label,
							  (size_t)label_width, 0);
		nb_buf_printf(&row, "  %s%s%*s  %s", branch, label,
			      label_width - (int)nb_utf8_width_cstr(label), "", node->description);
	}
	if (!nb_buf_ok(&row))
		return NB_ERR_NOMEM;
	char *clipped NB_CLEANUP(nb_charp_cleanup) = malloc(row.len + 4);
	if (!clipped)
		return NB_ERR_NOMEM;
	nt_terminal_copy_ellipsized_display_width(clipped, row.len + 4, nb_buf_cstr(&row),
						  (size_t)width, 0);
	return nb_buf_append_cstr(out, clipped) ? 0 : NB_ERR_NOMEM;
}

static int cli_skill_tree_populate(struct nt_tree_view_state *state,
				   const struct cli_skill_list_row *rows, size_t count)
{
	state->format_row = cli_skill_tree_row;
	for (size_t i = 0; i < count; ++i) {
		const char *marker = rows[i].state == CLI_SKILL_LIST_DISABLED ? "×"
				     : rows[i].state == CLI_SKILL_LIST_ACTIVE ? CLI_FEED_DONE
									      : CLI_FEED_PENDING;
		nb_buf label NB_CLEANUP(nb_buf_cleanup) = {0};
		if (!nb_buf_printf(&label, "%s %s", marker, rows[i].name))
			return NB_ERR_NOMEM;
		if (nt_tree_model_add(state->model, SIZE_MAX, nb_buf_cstr(&label),
				      rows[i].description) == SIZE_MAX)
			return state->model->error;
	}
	int rc = nt_tree_view_state_update(state, 0, count);
	return rc < 0 ? rc : 0;
}

struct nt_view *cli_skill_tree_add(struct nt_builder *builder, struct nt_view *parent,
				   const struct cli_skill_list_row *rows, size_t count)
{
	struct nt_view *tree = nt_tree_view_add(builder, parent, 0, NULL);
	if (tree) {
		nt_builder_fail(builder, nt_view_set_focusable(tree, 0));
		nt_builder_fail(builder,
				cli_skill_tree_populate(nt_tree_view_state(tree), rows, count));
	}
	return tree;
}

int cli_skill_tree_render(nb_buf *out, int width, const struct cli_skill_list_row *rows,
			  size_t count)
{
	struct nt_tree_model model NB_CLEANUP(nt_tree_model_deinit) = {0};
	struct nt_tree_view_state state NB_CLEANUP(nt_tree_view_state_deinit) = {0};
	int rc = nt_tree_model_init(&model);
	if (!rc)
		rc = nt_tree_view_state_init(&state, &model);
	if (!rc)
		rc = cli_skill_tree_populate(&state, rows, count);
	if (!rc)
		rc = nt_tree_view_state_render(&state, out, width, 0, SIZE_MAX);
	return rc;
}
