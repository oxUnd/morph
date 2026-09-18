#include "command_analyzer.h"
#include "util/array.h"
#include "util/buf.h"
#include "util/error.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct segment_array {
	morph_array_t items;
};

static char *trim_dup(const char *start, size_t len)
{
	while (len > 0 && isspace((unsigned char)start[0])) {
		start++;
		len--;
	}
	while (len > 0 && isspace((unsigned char)start[len - 1]))
		len--;
	return strndup(start, len);
}

static int segment_push(struct command_analysis *out,
			const char *start, size_t len)
{
	struct command_segment *segment;
	char *raw;
	morph_array_t words;
	morph_buf_t word;
	int quote = 0;
	int escaped = 0;
	int rc;

	raw = trim_dup(start, len);
	if (!raw)
		MORPH_RETURN(-ENOMEM);
	if (!raw[0]) {
		free(raw);
		return 0;
	}
	segment = realloc(out->segments,
		(out->count + 1) * sizeof(*out->segments));
	if (!segment) {
		free(raw);
		MORPH_RETURN(-ENOMEM);
	}
	out->segments = segment;
	segment = &out->segments[out->count++];
	memset(segment, 0, sizeof(*segment));
	segment->raw = raw;

	rc = morph_array_init(&words, 4, sizeof(char *));
	if (rc != 0)
		return rc;
	rc = morph_buf_init(&word, 64);
	if (rc != 0) {
		morph_array_cleanup(&words);
		return rc;
	}
	for (const char *p = raw;; p++) {
		unsigned char ch = (unsigned char)*p;
		int at_end = ch == '\0';

		if (escaped) {
			rc = morph_buf_putc(&word, (char)ch);
			escaped = 0;
			if (rc != 0)
				break;
			continue;
		}
		if (ch == '\\' && quote != '\'') {
			escaped = 1;
			continue;
		}
		if (quote != 0) {
			if (ch == (unsigned char)quote)
				quote = 0;
			else if (morph_buf_putc(&word, (char)ch) != 0)
				break;
			continue;
		}
		if (ch == '\'' || ch == '"') {
			quote = ch;
			continue;
		}
		if (at_end || isspace(ch)) {
			if (word.len > 0) {
				char **slot = morph_array_push(&words);

				if (!slot) {
					rc = -ENOMEM;
					break;
				}
				*slot = strdup(morph_buf_cstr(&word));
				if (!*slot) {
					rc = -ENOMEM;
					break;
				}
				morph_buf_reset(&word);
			}
			if (at_end)
				break;
			continue;
		}
		if (morph_buf_putc(&word, (char)ch) != 0) {
			rc = -ENOMEM;
			break;
		}
	}
	morph_buf_cleanup(&word);
	if (quote != 0 || escaped || rc != 0) {
		for (size_t i = 0; i < words.nelts; i++)
			free(*(char **)morph_array_get(&words, i));
		morph_array_cleanup(&words);
		return rc != 0 ? rc : 0;
	}
	segment->argv = words.elts;
	segment->argc = words.nelts;
	segment->parsed = segment->argc > 0;
	words.elts = NULL;
	morph_array_cleanup(&words);
	return 0;
}

int command_analyze(const char *command, struct command_analysis *out)
{
	const char *start;
	int quote = 0;
	int escaped = 0;

	if (!command || !out)
		MORPH_RETURN(-EINVAL);
	memset(out, 0, sizeof(*out));
	start = command;
	for (const char *p = command;; p++) {
		unsigned char ch = (unsigned char)*p;

		if (escaped) {
			escaped = 0;
			continue;
		}
		if (ch == '\\' && quote != '\'') {
			escaped = 1;
			continue;
		}
		if (ch == '$' && p[1] == '(')
			out->complex = 1;
		if (ch == '`' || (ch == '<' && p[1] == '<') ||
			(ch == '>' && p[1] == '>'))
			out->complex = 1;
		if (quote != 0) {
			if (ch == (unsigned char)quote)
				quote = 0;
			continue;
		}
		if (ch == '\'' || ch == '"') {
			quote = ch;
			continue;
		}
		if (ch == ';' || ch == '|' || ch == '&' || ch == '\n' ||
			ch == '\0') {
			size_t len = (size_t)(p - start);

			if (segment_push(out, start, len) != 0) {
				command_analysis_cleanup(out);
				MORPH_RETURN(-ENOMEM);
			}
			if (ch == '\0')
				break;
			if (ch == '|' || ch == '&') {
				if (p[1] == ch)
					p++;
			}
			start = p + 1;
		}
	}
	if (quote != 0 || escaped)
		out->complex = 1;
	return 0;
}

void command_analysis_cleanup(struct command_analysis *analysis)
{
	if (!analysis)
		return;
	for (size_t i = 0; i < analysis->count; i++) {
		struct command_segment *segment = &analysis->segments[i];

		free(segment->raw);
		for (size_t j = 0; j < segment->argc; j++)
			free(segment->argv[j]);
		free(segment->argv);
	}
	free(analysis->segments);
	memset(analysis, 0, sizeof(*analysis));
}
