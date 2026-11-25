#include "sapi/cli/presentation/diff.h"

#include "terminal/text.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/utf8.h>
#include <nothing/tui/theme.h>
#include <nothing/tui/views/text_view.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define CLI_DIFF_LIMIT (4u * 1024u * 1024u)

struct cli_diff_line {
	size_t offset;
	enum nt_theme_role role;
};

static int cli_diff_number(const char *text, long long *number)
{
	char *end = NULL;
	errno = 0;
	long long value = strtoll(text, &end, 10);
	if (errno == ERANGE || end == text || value < 0)
		return 0;

	*number = value;
	return 1;
}

static int cli_diff_line_add(nb_buf *text, nb_vec *lines, enum nt_theme_role role, const char *line,
			     size_t length, size_t *position)
{
	const struct cli_diff_line entry = {.offset = text->len, .role = role};
	if (!nb_vec_push_copy(lines, &entry) || !nb_buf_append(text, line, length) ||
	    !nb_buf_append_cstr(text, "\n") || !nb_buf_append_char(text, 0))
		return NB_ERR_NOMEM;

	*position += length + 1;
	return NB_OK;
}

int cli_diff_present(struct nt_view *document, const char *patch, nb_vec *hunks, int styled)
{
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_vec lines NB_CLEANUP(nb_vec_cleanup) = {0};
	nb_vec spans NB_CLEANUP(nb_vec_cleanup) = {0};
	nb_vec positions NB_CLEANUP(nb_vec_cleanup) = {0};
	char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		patch, strlen(patch), MORPH_TERMINAL_TEXT_MULTILINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;

	nb_vec_init(&lines, sizeof(struct cli_diff_line));
	nb_vec_init(&spans, sizeof(struct nt_text_span));
	nb_vec_init(&positions, sizeof(size_t));
	long long old = 0, current = 0;
	int in_hunk = 0;
	size_t position = 0;
	const char *line = safe;
	while (*line) {
		const char *end = strchr(line, '\n');
		size_t length = end ? (size_t)(end - line) : strlen(line);
		nb_buf row NB_CLEANUP(nb_buf_cleanup) = {0};
		enum nt_theme_role role = NT_THEME_TEXT;
		if (!strncmp(line, "@@ ", 3)) {
			const char *plus = memchr(line, '+', length);
			in_hunk = !strncmp(line, "@@ -", 4) && cli_diff_number(line + 4, &old) &&
				  plus && cli_diff_number(plus + 1, &current);
			if (!nb_vec_push_copy(&positions, &position))
				return NB_ERR_NOMEM;

			role = NT_THEME_ACCENT;
		} else if (in_hunk && old < LLONG_MAX && current < LLONG_MAX && length &&
			   (line[0] == '+' || line[0] == '-' || line[0] == ' ')) {
			char before[32] = "", after[32] = "";
			if (line[0] != '+')
				(void)snprintf(before, sizeof(before), "%lld", old++);

			if (line[0] != '-')
				(void)snprintf(after, sizeof(after), "%lld", current++);

			if (!nb_buf_printf(&row, "%5s %5s %c ", before, after, line[0]) ||
			    !nb_buf_append(&row, line + 1, length - 1))
				return NB_ERR_NOMEM;

			role = line[0] == '+'	? NT_THEME_SUCCESS
			       : line[0] == '-' ? NT_THEME_ERROR
						: NT_THEME_TEXT;
		} else if (!strncmp(line, "diff ", 5) || !strncmp(line, "*** ", 4) ||
			   !strncmp(line, "--- ", 4) || !strncmp(line, "+++ ", 4)) {
			in_hunk = 0;
			role = NT_THEME_MUTED;
		}

		const char *rendered = row.len ? nb_buf_cstr(&row) : line;
		size_t rendered_length = row.len ? row.len : length;
		int truncated = rendered_length > CLI_DIFF_LIMIT - text.len;
		if (truncated)
			rendered_length = nb_utf8_clamp_bytes(rendered, CLI_DIFF_LIMIT - text.len);

		int rc = cli_diff_line_add(&text, &lines, role, rendered, rendered_length,
					   &position);
		if (rc)
			return rc;

		if (truncated || text.len >= CLI_DIFF_LIMIT) {
			const char *notice = "Diff preview truncated at 4 MiB.";
			rc = cli_diff_line_add(&text, &lines, NT_THEME_WARNING, notice,
					       strlen(notice), &position);
			if (rc)
				return rc;

			break;
		}

		line = end ? end + 1 : line + length;
	}

	for (size_t i = 0; i < lines.len; i++) {
		const struct cli_diff_line *entry = nb_vec_at(&lines, i);
		struct nt_text_span span = {.text = nb_buf_cstr(&text) + entry->offset};
		if (styled)
			span.style = nt_theme_style(nt_view_theme(document), entry->role,
						    NT_VISUAL_NORMAL);

		if (!nb_vec_push_copy(&spans, &span))
			return NB_ERR_NOMEM;
	}

	int rc = nt_text_view_set_limit(document, CLI_DIFF_LIMIT + 2u * 1024u * 1024u);
	if (!rc)
		rc = nt_text_view_set_spans(document, (const struct nt_text_span *)spans.data,
					    spans.len);

	if (!rc) {
		nb_vec_clear(hunks);
		nb_vec_move(hunks, &positions);
	}

	return rc;
}
