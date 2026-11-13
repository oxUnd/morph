#include "sapi/cli/widgets/shell_style.h"
#include "nb/err.h"
#include "sapi/cli/application.h"
#include "sapi/cli/application_internal.h"

static int cli_shell_style_shell_operator(char ch)
{
	return ch && strchr("|&;()<>", ch) != NULL;
}

/* Recognize only a literal leading cd, never evaluate shell syntax. */
const char *cli_shell_summary_borrow(const char *text, const char *workdir)
{
	const char *p = text;
	nb_buf path NB_CLEANUP(nb_buf_cleanup) = {0};
	char quote = 0;
	int valid = 1;

	while (*p == ' ' || *p == '\t')
		p++;
	if (strncmp(p, "cd", 2) != 0 || (p[2] != ' ' && p[2] != '\t'))
		return text;
	p += 2;
	while (*p == ' ' || *p == '\t')
		p++;
	if (strncmp(p, "-- ", 3) == 0) {
		p += 3;
		while (*p == ' ' || *p == '\t')
			p++;
	}
	if ((nb_buf_init(&path), nb_buf_reserve(&path, 128) ? 0 : NB_ERR_NOMEM) != 0)
		return text;
	while (*p) {
		char ch = *p;

		if (!quote && (isspace((unsigned char)ch) || cli_shell_style_shell_operator(ch)))
			break;
		p++;
		if (ch == quote) {
			quote = 0;
		} else if (!quote && (ch == '\'' || ch == '"')) {
			quote = ch;
		} else if (quote != '\'' && strchr("$`\\*?~[]", ch)) {
			valid = 0;
			break;
		} else {
			(void)nb_buf_append_char(&path, ch);
		}
	}
	valid = valid && !quote && !nb_buf_status(&path) && path.len &&
		(strcmp(path.ptr, workdir) == 0 || strcmp(path.ptr, ".") == 0);
	nb_buf_cleanup(&path);
	while (*p == ' ' || *p == '\t')
		p++;
	if (valid && strncmp(p, "&&", 2) == 0)
		p += 2;
	else if (valid && *p == ';' && p[1] != ';' && p[1] != '&')
		p++;
	else
		return text;
	while (isspace((unsigned char)*p))
		p++;
	return *p ? p : text;
}

static int cli_shell_style_shell_assignment(const char *text, size_t len)
{
	size_t i = 0;

	if (!len || !(isalpha((unsigned char)text[0]) || text[0] == '_'))
		return 0;
	while (i < len && (isalnum((unsigned char)text[i]) || text[i] == '_'))
		i++;
	return i < len && text[i] == '=';
}

static void cli_shell_style_shell_span(nb_buf *out, const char *start, const char *end,
				       const char *color, int width, int *column)
{
	int span_width = 0;

	/* Prefer wrapping between words, falling back to codepoint wrapping for
	 * a single long word. Colors never participate in width calculations. */
	for (const char *p = start; p < end;) {
		int32_t cp;

		p = nb_utf8_next_cstr(p, &cp);
		span_width += nb_utf8_width_cp((unsigned)cp);
	}
	if (width > 0 && *column > 0 && span_width <= width && *column + span_width > width) {
		(void)nb_buf_append_cstr(out, "\n    ");
		*column = 0;
	}
	(void)nb_buf_append_cstr(out, color);
	while (start < end) {
		int32_t cp;
		const char *next = nb_utf8_next_cstr(start, &cp);
		int columns = nb_utf8_width_cp((unsigned)cp);

		if (cp == '\n' || (width > 0 && *column > 0 && *column + columns > width)) {
			(void)nb_buf_append_cstr(out, ANSI_RESET "\n    ");
			(void)nb_buf_append_cstr(out, color);
			*column = 0;
		}
		if (cp != '\n') {
			(void)nb_buf_append(out, start, (size_t)(next - start));
			*column += columns;
		}
		start = next;
	}
	if (color[0])
		(void)nb_buf_append_cstr(out, ANSI_RESET);
}

int cli_shell_style(nb_buf *out, const char *text, int width)
{
	char *safe = morph_terminal_sanitize_dup(text, strlen(text), MORPH_TERMINAL_TEXT_MULTILINE,
						 NULL);
	const char *p = safe;
	int command = 1;
	int redirect = 0;
	int column = 0;

	if (!safe)
		return NB_ERR_NOMEM;
	while (*p) {
		const char *start = p;
		const char *color = "";

		if (isspace((unsigned char)*p)) {
			do {
				if (*p == '\n')
					command = 1;
				p++;
			} while (*p && isspace((unsigned char)*p));
		} else if (*p == '#') {
			color = ANSI_DIM;
			p += strcspn(p, "\n");
		} else if (cli_shell_style_shell_operator(*p)) {
			color = ANSI_YELLOW;
			redirect = *p == '<' || *p == '>';
			if (!redirect)
				command = 1;
			p++;
			if (*p == start[0] || (redirect && *p == '&'))
				p++;
		} else {
			/* Quotes/escapes stay within a word; operators inside them
			 * must not start a new command. Unknown syntax stays literal. */
			while (*p && !isspace((unsigned char)*p) &&
			       !cli_shell_style_shell_operator(*p)) {
				if (*p == '\\' && p[1]) {
					p += 2;
				} else if (*p == '\'' || *p == '"') {
					char quote = *p++;

					while (*p && *p != quote) {
						if (quote == '"' && *p == '\\' && p[1])
							p++;
						p++;
					}
					if (*p)
						p++;
				} else {
					p++;
				}
			}
			if (*start == '\'' || *start == '"')
				color = ANSI_GREEN;
			else if (command && !redirect &&
				 !cli_shell_style_shell_assignment(start, (size_t)(p - start)))
				color = ANSI_CYAN;
			if (!redirect &&
			    !cli_shell_style_shell_assignment(start, (size_t)(p - start)))
				command = 0;
			redirect = 0;
		}
		cli_shell_style_shell_span(out, start, p, color, width, &column);
	}
	free(safe);
	if (nb_buf_status(out))
		return nb_buf_status(out);
	return 0;
}
