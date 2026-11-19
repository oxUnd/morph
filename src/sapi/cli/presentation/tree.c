#include "sapi/cli/presentation/tree.h"

#include <nb/utf8.h>
#include <nb/cleanup.h>
#include "sapi/cli/presentation/terminal_text.h"
#include "terminal/text.h"
#include <stdlib.h>
#include <string.h>

const char *cli_tree_branch(int is_last, int continuation)
{
	return continuation ? (is_last ? "  " : "│ ") : (is_last ? "└ " : "├ ");
}

int cli_tree_prefix(nb_buf *output, const int *ancestors_last, size_t depth, int is_last,
		    int continuation, size_t indent)
{
	if (!output || (depth && !ancestors_last))
		return NB_ERR_INVAL;

	for (size_t i = 0; i < indent; i++)
		nb_buf_append_char(output, ' ');

	for (size_t i = 0; i < depth; i++)
		nb_buf_append_cstr(output, cli_tree_branch(ancestors_last[i], 1));

	nb_buf_append_cstr(output, cli_tree_branch(is_last, continuation));
	return nb_buf_status(output);
}

static char *cli_tree_clip(const char *text, size_t columns)
{
	size_t capacity = strlen(text) + sizeof("…");
	char *clipped = malloc(capacity);
	if (clipped)
		morph_terminal_copy_ellipsized_display_width(clipped, capacity, text, columns, 0);

	return clipped;
}

static int cli_tree_append_line(nb_buf *output, const nb_buf *line, size_t columns)
{
	if (!nb_buf_ok(line))
		return nb_buf_status(line);

	const char *text = nb_buf_cstr(line);
	char *plain NB_CLEANUP(nb_charp_cleanup) = NULL;
	char *clipped NB_CLEANUP(nb_charp_cleanup) = NULL;
	if (cli_terminal_text_width(text) > columns) {
		plain = cli_terminal_text_strip_ansi_dup(text, nb_buf_len(line), NULL);
		clipped = plain ? cli_tree_clip(plain, columns) : NULL;
		if (!clipped)
			return NB_ERR_NOMEM;

		text = clipped;
	}

	nb_buf_printf(output, "%s\n", text);
	return nb_buf_status(output);
}

int cli_tree_row(nb_buf *output, const char *ancestor, int is_last, const char *marker,
		 const char *label, const char *description, int label_width, int columns,
		 const char *style, const char *reset)
{
	if (!output || !ancestor || !marker || !label || !description || !style || !reset ||
	    columns < 1)
		return NB_ERR_INVAL;

	char *safe NB_CLEANUP(nb_charp_cleanup) = NULL;
	char *detail NB_CLEANUP(nb_charp_cleanup) = NULL;
	safe = morph_terminal_sanitize_dup(label, strlen(label), MORPH_TERMINAL_TEXT_SINGLE_LINE,
					   NULL);
	detail = morph_terminal_sanitize_dup(description, strlen(description),
					     MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe || !detail)
		return NB_ERR_NOMEM;

	size_t width = (size_t)columns;
	size_t prefix = 4 + cli_terminal_text_width(ancestor);
	size_t mark = *marker ? cli_terminal_text_width(marker) + 1 : 0;
	size_t available = prefix + mark < width ? width - prefix - mark : 0;
	size_t name_width = nb_utf8_width_cstr(safe);
	if (label_width > 0 && (size_t)label_width > name_width)
		name_width = (size_t)label_width;

	if (!name_width || name_width > available)
		name_width = available;

	char *name NB_CLEANUP(nb_charp_cleanup) = cli_tree_clip(safe, name_width);
	if (!name)
		return NB_ERR_NOMEM;

	nb_buf line NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf_printf(&line, "  %s%s%s%s%s%s%s", ancestor, cli_tree_branch(is_last, 0), marker,
		      *marker ? " " : "", style, name, reset);
	if (!*detail)
		return cli_tree_append_line(output, &line, width);

	size_t space = available - name_width;
	int inline_detail = space >= 20;
	size_t detail_width =
		inline_detail ? space - 2 : (width > prefix + 2 ? width - prefix - 2 : 0);
	char *clipped NB_CLEANUP(nb_charp_cleanup) = cli_tree_clip(detail, detail_width);
	if (!clipped)
		return NB_ERR_NOMEM;

	if (inline_detail) {
		int padding = (int)(name_width - nb_utf8_width_cstr(name));
		nb_buf_printf(&line, "%*s  %s", padding, "", clipped);
	} else {
		int rc = cli_tree_append_line(output, &line, width);
		if (rc)
			return rc;

		nb_buf_clear(&line);
		nb_buf_printf(&line, "  %s%s  %s", ancestor, cli_tree_branch(is_last, 1), clipped);
	}

	return cli_tree_append_line(output, &line, width);
}
