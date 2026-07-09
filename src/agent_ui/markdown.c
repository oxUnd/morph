#include "agent_ui.h"

#include "util/buf.h"
#include "util/utf8.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

enum markdown_line_kind {
	MD_LINE_BLANK = 0,
	MD_LINE_PARAGRAPH,
	MD_LINE_HEADING,
	MD_LINE_LIST,
	MD_LINE_QUOTE,
	MD_LINE_FENCE,
	MD_LINE_TABLE,
	MD_LINE_MATH
};

static int decode_cp(const char *s, size_t len, size_t off,
		     unsigned *cp, size_t *cp_len)
{
	if (!s || off >= len || !cp || !cp_len)
		return 0;
	return utf8_decode_codepoint(s + off, len - off, cp, cp_len);
}

static int fullwidth_markdown_char(unsigned cp, char *out)
{
	switch (cp) {
	case 0x3000: *out = ' '; return 1;
	case 0xFF01: *out = '!'; return 1;
	case 0xFF03: *out = '#'; return 1;
	case 0xFF04: *out = '$'; return 1;
	case 0xFF08: *out = '('; return 1;
	case 0xFF09: *out = ')'; return 1;
	case 0xFF0A: *out = '*'; return 1;
	case 0xFF0B: *out = '+'; return 1;
	case 0xFF0D: *out = '-'; return 1;
	case 0xFF0E: *out = '.'; return 1;
	case 0xFF1E: *out = '>'; return 1;
	case 0xFF3B: *out = '['; return 1;
	case 0xFF3D: *out = ']'; return 1;
	case 0xFF3F: *out = '_'; return 1;
	case 0xFF40: *out = '`'; return 1;
	case 0xFF5B: *out = '{'; return 1;
	case 0xFF5C: *out = '|'; return 1;
	case 0xFF5D: *out = '}'; return 1;
	case 0xFF5E: *out = '~'; return 1;
	default: return 0;
	}
}

static int fullwidth_table_sep_char(unsigned cp, char *out)
{
	if (fullwidth_markdown_char(cp, out))
		return 1;
	if (cp == 0xFF1A) {
		*out = ':';
		return 1;
	}
	return 0;
}

static int append_normalized_chars(morph_buf_t *out, const char *s,
				   size_t len, int table_sep)
{
	size_t i = 0;

	while (i < len) {
		unsigned cp;
		size_t cp_len;
		char repl;

		if (decode_cp(s, len, i, &cp, &cp_len)) {
			if ((table_sep ?
			     fullwidth_table_sep_char(cp, &repl) :
			     fullwidth_markdown_char(cp, &repl))) {
				if (morph_buf_putc(out, repl) < 0)
					return -1;
				i += cp_len;
				continue;
			}
		}
		if (morph_buf_putc(out, s[i]) < 0)
			return -1;
		i++;
	}
	return 0;
}

static size_t skip_indent(const char *line, size_t len)
{
	size_t i = 0;

	while (i < len) {
		unsigned cp;
		size_t cp_len;

		if (line[i] == ' ' || line[i] == '\t') {
			i++;
			continue;
		}
		if (decode_cp(line, len, i, &cp, &cp_len) && cp == 0x3000) {
			i += cp_len;
			continue;
		}
		break;
	}
	return i;
}

static int line_fence_info(const char *line, size_t len,
			   char *marker, int *count)
{
	size_t i = skip_indent(line, len);
	char fc = 0;
	int n = 0;

	if (i >= len)
		return 0;
	if (line[i] == '`' || line[i] == '~') {
		fc = line[i];
		while (i < len && line[i] == fc) {
			n++;
			i++;
		}
	} else {
		unsigned cp;
		size_t cp_len;

		if (!decode_cp(line, len, i, &cp, &cp_len))
			return 0;
		if (cp == 0xFF40)
			fc = '`';
		else if (cp == 0xFF5E)
			fc = '~';
		else
			return 0;
		while (i < len) {
			unsigned cp2;
			size_t cp2_len;

			if (!decode_cp(line, len, i, &cp2, &cp2_len) ||
			    cp2 != cp)
				break;
			n++;
			i += cp2_len;
		}
	}
	if (n < 3)
		return 0;
	*marker = fc;
	*count = n;
	return 1;
}

static int line_has_pipe(const char *line, size_t len)
{
	size_t i = 0;

	while (i < len) {
		unsigned cp;
		size_t cp_len;

		if (line[i] == '|')
			return 1;
		if (decode_cp(line, len, i, &cp, &cp_len)) {
			if (utf8_is_fullwidth_pipe_cp(cp))
				return 1;
			i += cp_len;
		} else {
			i++;
		}
	}
	return 0;
}

static int line_is_table_sep(const char *line, size_t len)
{
	int has_pipe = 0;
	int has_dash = 0;
	size_t i = 0;

	while (i < len) {
		unsigned char c = (unsigned char)line[i];
		unsigned cp;
		size_t cp_len;

		if (c == '|') {
			has_pipe = 1;
			i++;
		} else if (c == '-') {
			has_dash = 1;
			i++;
		} else if (c == ':' || c == ' ' || c == '\t') {
			i++;
		} else if (decode_cp(line, len, i, &cp, &cp_len)) {
			if (utf8_is_fullwidth_pipe_cp(cp)) {
				has_pipe = 1;
				i += cp_len;
			} else if (utf8_is_fullwidth_dash_cp(cp)) {
				has_dash = 1;
				i += cp_len;
			} else if (utf8_is_fullwidth_colon_cp(cp)) {
				i += cp_len;
			} else {
				return 0;
			}
		} else {
			return 0;
		}
	}
	return has_pipe && has_dash;
}

static int line_split_trailing_table_text(const char *line, size_t len,
					  size_t *table_len,
					  size_t *text_start)
{
	size_t i = skip_indent(line, len);
	size_t last_pipe = (size_t)-1;
	int pipes = 0;

	if (i >= len || line[i] != '|')
		return 0;
	for (; i < len; i++) {
		if (line[i] == '|') {
			last_pipe = i;
			pipes++;
		}
	}
	if (pipes < 3 || last_pipe == (size_t)-1)
		return 0;
	i = last_pipe + 1;
	while (i < len && (line[i] == ' ' || line[i] == '\t'))
		i++;
	if (i >= len)
		return 0;
	*table_len = last_pipe + 1;
	*text_start = i;
	return 1;
}

static int line_is_heading(const char *line, size_t len)
{
	size_t i = skip_indent(line, len);
	int hashes = 0;

	while (i < len && line[i] == '#') {
		hashes++;
		i++;
	}
	return hashes > 0 && hashes <= 6 && i < len &&
	       isspace((unsigned char)line[i]);
}

static int line_is_list_item(const char *line, size_t len)
{
	size_t i = skip_indent(line, len);

	if (i + 1 < len &&
	    (line[i] == '-' || line[i] == '*' || line[i] == '+') &&
	    isspace((unsigned char)line[i + 1]))
		return 1;
	while (i < len && isdigit((unsigned char)line[i]))
		i++;
	return i + 1 < len && line[i] == '.' &&
	       isspace((unsigned char)line[i + 1]);
}

static int line_is_quote(const char *line, size_t len)
{
	size_t i = skip_indent(line, len);

	return i < len && line[i] == '>';
}

static int line_is_math_block(const char *line, size_t len)
{
	size_t i = skip_indent(line, len);

	return i + 1 < len && line[i] == '$' && line[i + 1] == '$';
}

static int line_is_horizontal_rule(const char *line, size_t len)
{
	size_t i = skip_indent(line, len);
	char marker;
	int count = 0;

	if (i >= len || (line[i] != '-' && line[i] != '*' && line[i] != '_'))
		return 0;
	marker = line[i];
	while (i < len) {
		if (line[i] == marker) {
			count++;
			i++;
			continue;
		}
		if (line[i] == ' ' || line[i] == '\t') {
			i++;
			continue;
		}
		return 0;
	}
	return count >= 3;
}

static enum markdown_line_kind classify_line(const char *line, size_t len,
					     int is_fence)
{
	if (len == 0)
		return MD_LINE_BLANK;
	if (is_fence)
		return MD_LINE_FENCE;
	if (line_is_heading(line, len))
		return MD_LINE_HEADING;
	if (line_is_list_item(line, len))
		return MD_LINE_LIST;
	if (line_is_quote(line, len))
		return MD_LINE_QUOTE;
	if (line_is_math_block(line, len))
		return MD_LINE_MATH;
	if (line_has_pipe(line, len))
		return MD_LINE_TABLE;
	return MD_LINE_PARAGRAPH;
}

static int needs_blank_before(enum markdown_line_kind kind,
			      enum markdown_line_kind prev_kind,
			      int previous_blank, int out_has_content)
{
	if (!out_has_content || previous_blank)
		return 0;
	if (kind == MD_LINE_BLANK || kind == MD_LINE_PARAGRAPH)
		return 0;
	if (kind == MD_LINE_LIST || kind == MD_LINE_QUOTE ||
	    kind == MD_LINE_TABLE)
		return prev_kind != kind;
	return 1;
}

static int append_fence_close(morph_buf_t *out, char marker, int len)
{
	if (morph_buf_putc(out, '\n') < 0)
		return -1;
	for (int i = 0; i < len; i++) {
		if (morph_buf_putc(out, marker) < 0)
			return -1;
	}
	return morph_buf_putc(out, '\n');
}

static int append_normalized_line(morph_buf_t *out, const char *line,
				  size_t len, int *previous_blank,
				  enum markdown_line_kind *prev_kind,
				  int *in_fence, char *fence_marker,
				  int *fence_len)
{
	morph_buf_t normalized;
	enum markdown_line_kind kind;
	char line_fence_marker = 0;
	int line_fence_len = 0;
	int was_in_fence = *in_fence;
	int is_fence;
	int table_sep;
	int normalize_body;
	int rc = 0;

	is_fence = line_fence_info(line, len, &line_fence_marker,
				   &line_fence_len);
	table_sep = !was_in_fence && line_is_table_sep(line, len);
	normalize_body = !was_in_fence || is_fence;

	if (morph_buf_init(&normalized, len + 1) < 0)
		return -1;
	if (normalize_body) {
		if (append_normalized_chars(&normalized, line, len,
					    table_sep) < 0) {
			rc = -1;
			goto out;
		}
	} else if (morph_buf_append(&normalized, line, len) < 0) {
		rc = -1;
		goto out;
	}

	line = morph_buf_cstr(&normalized);
	len = normalized.len;
	if (!was_in_fence && !line_is_table_sep(line, len)) {
		size_t table_len;
		size_t text_start;

		if (line_split_trailing_table_text(line, len, &table_len,
						   &text_start)) {
			if (append_normalized_line(out, line, table_len,
						   previous_blank, prev_kind,
						   in_fence, fence_marker,
						   fence_len) < 0 ||
			    append_normalized_line(out, line + text_start,
						   len - text_start,
						   previous_blank, prev_kind,
						   in_fence, fence_marker,
						   fence_len) < 0) {
				rc = -1;
			}
			goto out;
		}
	}
	if (!was_in_fence && line_is_horizontal_rule(line, len)) {
		if (out->len > 0 && !*previous_blank) {
			if (morph_buf_putc(out, '\n') < 0) {
				rc = -1;
				goto out;
			}
		}
		*previous_blank = 1;
		*prev_kind = MD_LINE_BLANK;
		goto out;
	}
	kind = classify_line(line, len, is_fence);

	if (is_fence) {
		if (!*in_fence) {
			*in_fence = 1;
			*fence_marker = line_fence_marker;
			*fence_len = line_fence_len;
		} else if (line_fence_marker == *fence_marker &&
			   line_fence_len >= *fence_len) {
			*in_fence = 0;
		}
	}
	if (len == 0) {
		if (out->len > 0 && !*previous_blank) {
			if (morph_buf_putc(out, '\n') < 0) {
				rc = -1;
				goto out;
			}
		}
		*previous_blank = 1;
		*prev_kind = MD_LINE_BLANK;
		goto out;
	}
	if (!was_in_fence && needs_blank_before(kind, *prev_kind,
						*previous_blank,
						out->len > 0)) {
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
	*previous_blank = 0;
	*prev_kind = kind;
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
	int fence_len = 3;
	enum markdown_line_kind prev_kind = MD_LINE_BLANK;

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
		while (line_len > 0 &&
		       (s[start + line_len - 1] == ' ' ||
			s[start + line_len - 1] == '\t'))
			line_len--;
		if (append_normalized_line(&out, s + start, line_len,
					   &previous_blank, &prev_kind,
					   &in_fence, &fence_marker,
					   &fence_len) < 0) {
			morph_buf_cleanup(&out);
			return NULL;
		}
		if (pos < len && s[pos] == '\r')
			pos++;
		if (pos < len && s[pos] == '\n')
			pos++;
	}
	if (in_fence && append_fence_close(&out, fence_marker, fence_len) < 0) {
		morph_buf_cleanup(&out);
		return NULL;
	}
	return morph_buf_detach(&out);
}
