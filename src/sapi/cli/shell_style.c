#include "sapi/cli/internal.h"
#include "sapi/cli/shell_style.h"

static int shell_operator(char ch)
{
	return ch && strchr("|&;()<>", ch) != NULL;
}

/* Recognize only a literal leading cd, never evaluate shell syntax. */
const char *cli_shell_summary(const char *text, const char *workdir)
{
	const char *p = text;
	morph_buf_t path;
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
	if (morph_buf_init(&path, 128) != 0)
		return text;
	while (*p) {
		char ch = *p;

		if (!quote && (isspace((unsigned char)ch) || shell_operator(ch)))
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
			(void)morph_buf_putc(&path, ch);
		}
	}
	valid = valid && !quote && !path.failed && path.len &&
		(strcmp(path.data, workdir) == 0 || strcmp(path.data, ".") == 0);
	morph_buf_cleanup(&path);
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

static int shell_assignment(const char *text, size_t len)
{
	size_t i = 0;

	if (!len || !(isalpha((unsigned char)text[0]) || text[0] == '_'))
		return 0;
	while (i < len && (isalnum((unsigned char)text[i]) || text[i] == '_'))
		i++;
	return i < len && text[i] == '=';
}

static void shell_span(morph_buf_t *out, const char *start, const char *end,
		       const char *color, int width, int *column)
{
	int span_width = 0;

	/* Prefer wrapping between words, falling back to codepoint wrapping for
	 * a single long word. Colors never participate in width calculations. */
	for (const char *p = start; p < end;) {
		utf8_int32_t cp;

		p = utf8codepoint(p, &cp);
		span_width += utf8_codepoint_width((unsigned)cp);
	}
	if (width > 0 && *column > 0 && span_width <= width &&
	    *column + span_width > width) {
		(void)morph_buf_puts(out, "\n    ");
		*column = 0;
	}
	(void)morph_buf_puts(out, color);
	while (start < end) {
		utf8_int32_t cp;
		const char *next = utf8codepoint(start, &cp);
		int columns = utf8_codepoint_width((unsigned)cp);

		if (cp == '\n' || (width > 0 && *column > 0 &&
		    *column + columns > width)) {
			(void)morph_buf_puts(out, ANSI_RESET "\n    ");
			(void)morph_buf_puts(out, color);
			*column = 0;
		}
		if (cp != '\n') {
			(void)morph_buf_append(out, start, (size_t)(next - start));
			*column += columns;
		}
		start = next;
	}
	if (color[0])
		(void)morph_buf_puts(out, ANSI_RESET);
}

int cli_shell_style(morph_buf_t *out, const char *text, int width)
{
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text),
		UTF8_TERMINAL_TEXT_MULTILINE, NULL);
	const char *p = safe;
	int command = 1;
	int redirect = 0;
	int column = 0;

	if (!safe)
		MORPH_RETURN(-ENOMEM);
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
		} else if (shell_operator(*p)) {
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
			while (*p && !isspace((unsigned char)*p) && !shell_operator(*p)) {
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
				 !shell_assignment(start, (size_t)(p - start)))
				color = ANSI_CYAN;
			if (!redirect && !shell_assignment(start, (size_t)(p - start)))
				command = 0;
			redirect = 0;
		}
		shell_span(out, start, p, color, width, &column);
	}
	free(safe);
	if (out->failed)
		MORPH_RETURN(out->failed);
	return 0;
}
