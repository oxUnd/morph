#include "read_lines.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/err.h"
#include "nb/file.h"
#include "nb/str.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_MAX_BYTES 65536UL
#define HARD_MAX_BYTES 262144UL
#define MAX_LINE_COUNT 1000LL

struct read_args {
	nb_buf path;
	long long start_line;
	long long line_count;
	int include_line_numbers;
	size_t max_bytes;
};

static void read_args_cleanup(struct read_args *args)
{
	nb_buf_destroy(&args->path);
}

static void read_file_cleanup(FILE **owner)
{
	if (owner && *owner)
		fclose(*owner);
}

static const char *skip_ws(const char *p)
{
	while (*p && isspace((unsigned char)*p))
		p++;
	return p;
}

static const char *find_key(const char *json, const char *key)
{
	size_t key_len;
	const char *p;

	key_len = strlen(key);
	p = json;
	while ((p = strchr(p, '"')) != NULL) {
		const char *start = p + 1;
		const char *end = start;

		while (*end) {
			if (*end == '\\' && end[1]) {
				end += 2;
				continue;
			}
			if (*end == '"')
				break;
			end++;
		}
		if (*end != '"')
			return NULL;
		if ((size_t)(end - start) == key_len &&
		    strncmp(start, key, key_len) == 0) {
			const char *after = skip_ws(end + 1);
			if (*after == ':')
				return skip_ws(after + 1);
		}
		p = end + 1;
	}
	return NULL;
}

static int json_parse_string_contents(const char *p, nb_buf *buf)
{
	while (*p && *p != '"') {
		if ((unsigned char)*p < 0x20) {
			return NB_ERR_INVAL;
		}
		if (*p == '\\') {
			p++;
			switch (*p) {
			case '"':
			case '\\':
			case '/':
				if (!nb_buf_append_char(buf, *p))
					return NB_ERR_NOMEM;
				break;
			case 'b':
				if (!nb_buf_append_char(buf, '\b'))
					return NB_ERR_NOMEM;
				break;
			case 'f':
				if (!nb_buf_append_char(buf, '\f'))
					return NB_ERR_NOMEM;
				break;
			case 'n':
				if (!nb_buf_append_char(buf, '\n'))
					return NB_ERR_NOMEM;
				break;
			case 'r':
				if (!nb_buf_append_char(buf, '\r'))
					return NB_ERR_NOMEM;
				break;
			case 't':
				if (!nb_buf_append_char(buf, '\t'))
					return NB_ERR_NOMEM;
				break;
			case 'u':
				if (!isxdigit((unsigned char)p[1]) ||
				    !isxdigit((unsigned char)p[2]) ||
				    !isxdigit((unsigned char)p[3]) ||
				    !isxdigit((unsigned char)p[4])) {
					return NB_ERR_INVAL;
				}
				if (!nb_buf_append_char(buf, '?'))
					return NB_ERR_NOMEM;
				p += 4;
				break;
			default:
				return NB_ERR_INVAL;
			}
			p++;
		} else {
			if (!nb_buf_append_char(buf, *p++))
				return NB_ERR_NOMEM;
		}
	}
	if (*p != '"') {
		return NB_ERR_INVAL;
	}
	return 1;
}

static int json_get_string(const char *json, const char *key, nb_buf *out)
{
	const char *p = find_key(json, key);

	if (!p)
		return 0;
	if (*p != '"')
		return NB_ERR_INVAL;
	return json_parse_string_contents(p + 1, out);
}

static int json_get_int(const char *json, const char *key, long long *out)
{
	const char *p;
	char *end;
	long long value;

	p = find_key(json, key);
	if (!p)
		return 0;
	errno = 0;
	value = strtoll(p, &end, 10);
	if (p == end || errno == ERANGE)
		return NB_ERR_INVAL;
	*out = value;
	return 1;
}

static int json_get_bool(const char *json, const char *key, int *out)
{
	const char *p;

	p = find_key(json, key);
	if (!p)
		return 0;
	if (strncmp(p, "true", 4) == 0) {
		*out = 1;
		return 1;
	}
	if (strncmp(p, "false", 5) == 0) {
		*out = 0;
		return 1;
	}
	return NB_ERR_INVAL;
}

static int parse_args(const char *json, struct read_args *args)
{
	long long max_bytes;
	int rc;

	args->max_bytes = DEFAULT_MAX_BYTES;

	rc = json_get_string(json, "path", &args->path);
	if (rc <= 0)
		return rc < 0 ? rc : NB_ERR_INVAL;
	rc = json_get_int(json, "start_line", &args->start_line);
	if (rc <= 0)
		return NB_ERR_INVAL;
	rc = json_get_int(json, "line_count", &args->line_count);
	if (rc <= 0)
		return NB_ERR_INVAL;
	rc = json_get_bool(json, "include_line_numbers",
			   &args->include_line_numbers);
	if (rc < 0)
		return NB_ERR_INVAL;
	rc = json_get_int(json, "max_bytes", &max_bytes);
	if (rc < 0)
		return NB_ERR_INVAL;
	if (rc > 0) {
		if (max_bytes <= 0)
			return NB_ERR_INVAL;
		if ((unsigned long long)max_bytes > HARD_MAX_BYTES)
			return NB_ERR_INVAL;
		args->max_bytes = (size_t)max_bytes;
	}

	if (args->start_line <= 0 || args->line_count <= 0 ||
	    args->line_count > MAX_LINE_COUNT)
		return NB_ERR_INVAL;
	if (args->start_line > LLONG_MAX - args->line_count)
		return NB_ERR_INVAL;
	return 0;
}

static int append_json_string(nb_buf *buf, const char *s)
{
	const unsigned char *p;

	if (!nb_buf_append_char(buf, '"'))
		return nb_buf_status(buf);
	for (p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"':
			if (!nb_buf_append_cstr(buf, "\\\""))
				return nb_buf_status(buf);
			break;
		case '\\':
			if (!nb_buf_append_cstr(buf, "\\\\"))
				return nb_buf_status(buf);
			break;
		case '\b':
			if (!nb_buf_append_cstr(buf, "\\b"))
				return nb_buf_status(buf);
			break;
		case '\f':
			if (!nb_buf_append_cstr(buf, "\\f"))
				return nb_buf_status(buf);
			break;
		case '\n':
			if (!nb_buf_append_cstr(buf, "\\n"))
				return nb_buf_status(buf);
			break;
		case '\r':
			if (!nb_buf_append_cstr(buf, "\\r"))
				return nb_buf_status(buf);
			break;
		case '\t':
			if (!nb_buf_append_cstr(buf, "\\t"))
				return nb_buf_status(buf);
			break;
		default:
			if (*p < 0x20) {
				if (!nb_buf_printf(buf, "\\u%04x", *p))
					return nb_buf_status(buf);
			} else if (!nb_buf_append_char(buf, (char)*p)) {
				return nb_buf_status(buf);
			}
			break;
		}
	}
	return nb_buf_append_char(buf, '"') ? NB_OK : nb_buf_status(buf);
}

static char *make_error(const char *message)
{
	nb_buf out NB_CLEANUP(nb_buf_destroy) = {0};

	if (!nb_buf_append_cstr(&out, "{\"error\":") ||
	    append_json_string(&out, message) != NB_OK || !nb_buf_append_char(&out, '}'))
		return strdup("{\"error\":\"out of memory\"}");
	return strdup(nb_buf_cstr(&out));
}

static int path_allowed(const char *path)
{
	const char *roots = getenv("MORPH_READ_LINES_ROOTS");
	nb_buf root NB_CLEANUP(nb_buf_destroy) = {0};

	if (roots && roots[0]) {
		nb_split_iter iter;
		nb_str token;

		nb_split_init(&iter, nb_str_from_cstr(roots), ':');
		nb_split_skip_empty(&iter, true);
		while (nb_split_next(&iter, &token)) {
			nb_buf_clear(&root);
			if (!nb_buf_append(&root, token.ptr, token.len))
				return nb_buf_status(&root);
			if (nb_path_is_within(path, nb_buf_cstr(&root), false))
				return 1;
		}
		return 0;
	}
	int rc = nb_path_canonical(&root, ".", NULL);
	if (rc == NB_ERR_NOMEM)
		return rc;
	if (rc == NB_OK && nb_path_is_within(path, nb_buf_cstr(&root), false))
		return 1;
	const char *home = getenv("HOME");
	if (!home || !home[0])
		return 0;
	const char *const names[] = {".morph", ".agents"};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		rc = nb_path_join(&root, home, names[i]);
		if (rc != NB_OK)
			return rc;
		if (nb_path_is_within(path, nb_buf_cstr(&root), false))
			return 1;
	}
	return 0;
}

static int skip_line(FILE *f)
{
	int c;

	while ((c = fgetc(f)) != EOF) {
		if (c == '\0')
			return NB_ERR_FORMAT;
		if (c == '\n')
			return 1;
	}
	if (ferror(f))
		return nb_status_from_errno(errno, NULL);
	return 0;
}

static int read_line_capped(FILE *f, nb_buf *line, size_t cap, int *overflow)
{
	int c;
	int saw_any;

	nb_buf_clear(line);
	*overflow = 0;
	saw_any = 0;
	while ((c = fgetc(f)) != EOF) {
		saw_any = 1;
		if (c == '\0')
			return NB_ERR_FORMAT;
		if (line->len + 1 <= cap) {
			if (!nb_buf_append_char(line, (char)c))
				return nb_buf_status(line);
		} else {
			*overflow = 1;
		}
		if (c == '\n')
			return 1;
	}
	if (ferror(f))
		return nb_status_from_errno(errno, NULL);
	return saw_any ? 1 : 0;
}

static int read_lines(FILE *f, const struct read_args *args, nb_buf *content, long long *end_line,
		      long long *next_line, int *eof, int *truncated)
{
	long long line_no;
	long long i;
	nb_buf line NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf prefix NB_CLEANUP(nb_buf_destroy) = {0};
	int rc;

	*end_line = 0;
	*next_line = args->start_line;
	*eof = 0;
	*truncated = 0;

	for (line_no = 1; line_no < args->start_line; line_no++) {
		rc = skip_line(f);
		if (rc < 0)
			return rc;
		if (rc == 0) {
			*eof = 1;
			*next_line = 0;
			return 0;
		}
	}

	for (i = 0; i < args->line_count; i++, line_no++) {
		int overflow;
		size_t prefix_len;
		size_t needed;

		rc = read_line_capped(f, &line, args->max_bytes, &overflow);
		if (rc < 0)
			return rc;
		if (rc == 0) {
			*eof = 1;
			*next_line = 0;
			break;
		}

		nb_buf_clear(&prefix);
		if (args->include_line_numbers && !nb_buf_printf(&prefix, "%lld: ", line_no))
			return nb_buf_status(&prefix);
		prefix_len = prefix.len;

		needed = prefix_len + line.len;
		if (overflow || content->len + needed > args->max_bytes) {
			*truncated = 1;
			*next_line = line_no;
			return 0;
		}

		if (!nb_buf_append(content, prefix.ptr, prefix.len) ||
		    !nb_buf_append(content, line.ptr, line.len))
			return nb_buf_status(content);

		*end_line = line_no;
		*next_line = line_no + 1;
	}

	if (!*eof && i >= args->line_count)
		*next_line = line_no;
	return 0;
}

static char *make_result(const char *path, const struct read_args *args, long long end_line,
			 long long next_line, int eof, int truncated, const char *content)
{
	nb_buf out NB_CLEANUP(nb_buf_destroy) = {0};

	(void)nb_buf_append_cstr(&out, "{\"path\":");
	int rc = append_json_string(&out, path);
	if (!rc &&
	    !nb_buf_printf(
		    &out,
		    ",\"start_line\":%lld,\"line_count\":%lld,\"end_line\":%lld,\"next_line\":",
		    args->start_line, args->line_count, end_line))
		rc = nb_buf_status(&out);
	if (!rc && !(next_line > 0 ? nb_buf_printf(&out, "%lld", next_line)
				   : nb_buf_append_cstr(&out, "null")))
		rc = nb_buf_status(&out);
	if (!rc && !nb_buf_printf(&out, ",\"eof\":%s,\"truncated\":%s,\"content\":",
				  eof ? "true" : "false", truncated ? "true" : "false"))
		rc = nb_buf_status(&out);
	if (!rc)
		rc = append_json_string(&out, content);
	if (!rc && !nb_buf_append_char(&out, '}'))
		rc = nb_buf_status(&out);
	return rc ? NULL : strdup(nb_buf_cstr(&out));
}

int ext_run(const char *args_json, char **result_json)
{
	struct read_args args NB_CLEANUP(read_args_cleanup) = {0};
	nb_buf path NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf content NB_CLEANUP(nb_buf_destroy) = {0};
	FILE *file NB_CLEANUP(read_file_cleanup) = NULL;
	long long end_line = 0, next_line = 0;
	int eof = 0, truncated = 0;

	if (!args_json || !result_json)
		return NB_ERR_INVAL;
	*result_json = NULL;
	int rc = parse_args(args_json, &args);
	if (rc != NB_OK) {
		*result_json = make_error("invalid arguments");
		return rc;
	}
	if (!nb_path_is_absolute(nb_buf_cstr(&args.path))) {
		*result_json = make_error("path must be absolute");
		return NB_ERR_INVAL;
	}
	rc = nb_path_canonical(&path, nb_buf_cstr(&args.path), NULL);
	if (rc != NB_OK) {
		*result_json = make_error("file not found");
		return rc;
	}
	rc = path_allowed(nb_buf_cstr(&path));
	if (rc <= 0) {
		*result_json = make_error("path is not under an allowed root");
		return rc < 0 ? rc : NB_ERR_PERMISSION;
	}
	nb_file_info info = {0};
	rc = nb_file_stat(nb_buf_cstr(&path), true, &info, NULL);
	if (rc != NB_OK || !info.is_regular) {
		*result_json = make_error("path is not a regular file");
		return rc != NB_OK ? rc : NB_ERR_INVAL;
	}
	file = fopen(nb_buf_cstr(&path), "rb");
	if (!file) {
		rc = nb_status_from_errno(errno, NULL);
		*result_json = make_error("failed to open file");
		return rc;
	}
	rc = read_lines(file, &args, &content, &end_line, &next_line, &eof, &truncated);
	if (rc != NB_OK) {
		*result_json = make_error(rc == NB_ERR_FORMAT ? "file appears to be binary"
							      : "failed to read file");
		return rc;
	}
	*result_json = make_result(nb_buf_cstr(&path), &args, end_line, next_line, eof, truncated,
				   nb_buf_cstr(&content));
	return *result_json ? NB_OK : NB_ERR_NOMEM;
}
