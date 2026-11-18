#include <morph/tui/terminal/report.h>
#include <nb/err.h>
#include <nb/utf8.h>
#include <morph/tui/terminal/text.h>
#include <nb/cleanup.h>
#include <stdlib.h>
#include <string.h>

const char *morph_terminal_tree_branch(int is_last, int continuation)
{
	return continuation ? (is_last ? "  " : "│ ") : (is_last ? "└ " : "├ ");
}

int morph_terminal_tree_prefix(nb_buf *output, const int *ancestors_last, size_t depth, int is_last,
			       int continuation, size_t indent)
{
	if (!output || (depth && !ancestors_last))
		return NB_ERR_INVAL;
	for (size_t i = 0; i < indent; i++)
		nb_buf_append_char(output, ' ');
	for (size_t i = 0; i < depth; i++)
		nb_buf_append_cstr(output, ancestors_last[i] ? "  " : "│ ");
	nb_buf_append_cstr(output, morph_terminal_tree_branch(is_last, continuation));
	return nb_buf_status(output);
}

static int report_append_line(nb_buf *output, const char *line, int columns)
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

int morph_terminal_tree_row(nb_buf *output, const char *ancestor, int is_last, const char *marker,
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
			      morph_terminal_tree_branch(is_last, 0), marker, marker[0] ? " " : "",
			      style, clipped, reset);
		int inline_detail = detail[0] && columns - prefix - mark - label_width - 2 >= 18;
		if (inline_detail) {
			morph_terminal_copy_ellipsized_display_width(
				clipped_detail, strlen(detail) + 4, detail,
				(size_t)(columns - prefix - mark - label_width - 2), 0);
			nb_buf_printf(&line, "%*s  %s", label_width - used, "", clipped_detail);
		}
		rc = report_append_line(output, nb_buf_cstr(&line), columns);
		if (detail[0] && !inline_detail) {
			int width = columns - prefix - 2;
			if (width < 0)
				width = 0;
			morph_terminal_copy_ellipsized_display_width(
				clipped_detail, strlen(detail) + 4, detail, (size_t)width, 0);
			nb_buf_clear(&line);
			nb_buf_printf(&line, "  %s%s  %s", ancestor,
				      morph_terminal_tree_branch(is_last, 1), clipped_detail);
			if (!rc)
				rc = report_append_line(output, nb_buf_cstr(&line), columns);
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
