#include "agent_ui.h"

#include "util/buf.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

struct replacement {
	const char *from;
	const char *to;
};

static const struct replacement markdown_replacements[] = {
	{ "＊", "*" },
	{ "＃", "#" },
	{ "～", "~" },
	{ "｀", "`" },
	{ "［", "[" },
	{ "］", "]" },
	{ "（", "(" },
	{ "）", ")" },
	{ "｛", "{" },
	{ "｝", "}" },
	{ "＿", "_" },
	{ "－", "-" },
	{ "　", " " },
	{ NULL, NULL }
};

static int append_normalized_chars(morph_buf_t *out, const char *s, size_t len)
{
	size_t i = 0;

	while (i < len) {
		int replaced = 0;

		for (int r = 0; markdown_replacements[r].from; r++) {
			size_t n = strlen(markdown_replacements[r].from);

			if (i + n <= len &&
			    strncmp(s + i, markdown_replacements[r].from, n) == 0) {
				if (morph_buf_puts(out,
					markdown_replacements[r].to) < 0)
					return -1;
				i += n;
				replaced = 1;
				break;
			}
		}
		if (replaced)
			continue;
		if (morph_buf_putc(out, s[i]) < 0)
			return -1;
		i++;
	}
	return 0;
}

static int line_starts_fence(const char *line, size_t len, char marker)
{
	size_t i = 0;
	int count = 0;

	while (i < len && (line[i] == ' ' || line[i] == '\t'))
		i++;
	while (i < len && line[i] == marker) {
		count++;
		i++;
	}
	return count >= 3;
}

static int line_is_list_item(const char *line, size_t len)
{
	size_t i = 0;

	while (i < len && (line[i] == ' ' || line[i] == '\t'))
		i++;
	if (i + 1 < len &&
	    (line[i] == '-' || line[i] == '*' || line[i] == '+') &&
	    isspace((unsigned char)line[i + 1]))
		return 1;
	while (i < len && isdigit((unsigned char)line[i]))
		i++;
	return i + 1 < len && line[i] == '.' &&
	       isspace((unsigned char)line[i + 1]);
}

static int line_is_heading(const char *line, size_t len)
{
	size_t i = 0;
	int hashes = 0;

	while (i < len && (line[i] == ' ' || line[i] == '\t'))
		i++;
	while (i < len && line[i] == '#') {
		hashes++;
		i++;
	}
	return hashes > 0 && hashes <= 6 && i < len &&
	       isspace((unsigned char)line[i]);
}

static int append_line_with_spacing(morph_buf_t *out, const char *line,
				    size_t len, int *previous_blank,
				    int *in_fence, char *fence_marker)
{
	morph_buf_t normalized;
	int is_fence;
	int needs_blank;
	int rc = 0;

	if (morph_buf_init(&normalized, len + 1) < 0)
		return -1;
	if (append_normalized_chars(&normalized, line, len) < 0) {
		morph_buf_cleanup(&normalized);
		return -1;
	}
	line = morph_buf_cstr(&normalized);
	len = normalized.len;
	is_fence = line_starts_fence(line, len, '`') ||
		   line_starts_fence(line, len, '~');
	if (is_fence) {
		*fence_marker = line_starts_fence(line, len, '~') ? '~' : '`';
		*in_fence = !*in_fence;
	}
	needs_blank = !*in_fence &&
		      (line_is_heading(line, len) || line_is_list_item(line, len));
	if (needs_blank && out->len > 0 && !*previous_blank) {
		if (morph_buf_putc(out, '\n') < 0) {
			rc = -1;
			goto out;
		}
	}
	if (morph_buf_append(out, line, len) < 0 ||
	    morph_buf_putc(out, '\n') < 0) {
		rc = -1;
		goto out;
	}
	*previous_blank = len == 0;
	(void)fence_marker;
out:
	morph_buf_cleanup(&normalized);
	return rc;
}

char *agent_ui_normalize_markdown(const char *input)
{
	morph_buf_t out;
	const char *s;
	size_t len;
	size_t pos = 0;
	int previous_blank = 1;
	int in_fence = 0;
	char fence_marker = '`';

	if (!input)
		input = "";
	len = strlen(input);
	if (morph_buf_init(&out, len + 32) < 0)
		return NULL;
	s = input;
	while (pos < len) {
		size_t start = pos;
		size_t line_len;

		while (pos < len && s[pos] != '\n' && s[pos] != '\r')
			pos++;
		line_len = pos - start;
		if (append_line_with_spacing(&out, s + start, line_len,
					     &previous_blank, &in_fence,
					     &fence_marker) < 0) {
			morph_buf_cleanup(&out);
			return NULL;
		}
		if (pos < len && s[pos] == '\r')
			pos++;
		if (pos < len && s[pos] == '\n')
			pos++;
	}
	if (in_fence) {
		if (morph_buf_putc(&out, '\n') < 0 ||
		    morph_buf_putc(&out, fence_marker) < 0 ||
		    morph_buf_putc(&out, fence_marker) < 0 ||
		    morph_buf_putc(&out, fence_marker) < 0 ||
		    morph_buf_putc(&out, '\n') < 0) {
			morph_buf_cleanup(&out);
			return NULL;
		}
	}
	return morph_buf_detach(&out);
}
