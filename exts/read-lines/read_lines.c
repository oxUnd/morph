#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_MAX_BYTES 65536UL
#define HARD_MAX_BYTES 262144UL
#define MAX_LINE_COUNT 1000LL

struct buf {
	char *data;
	size_t len;
	size_t cap;
};

struct read_args {
	char *path;
	long long start_line;
	long long line_count;
	int include_line_numbers;
	size_t max_bytes;
};

static char *xstrdup(const char *s)
{
	size_t len;
	char *copy;

	if (!s)
		return NULL;
	len = strlen(s);
	copy = malloc(len + 1);
	if (!copy)
		return NULL;
	memcpy(copy, s, len + 1);
	return copy;
}

static int buf_init(struct buf *buf, size_t cap)
{
	buf->data = malloc(cap);
	if (!buf->data)
		return -1;
	buf->data[0] = '\0';
	buf->len = 0;
	buf->cap = cap;
	return 0;
}

static void buf_free(struct buf *buf)
{
	if (!buf)
		return;
	free(buf->data);
	buf->data = NULL;
	buf->len = 0;
	buf->cap = 0;
}

static int buf_reserve(struct buf *buf, size_t need)
{
	size_t new_cap;
	char *new_data;

	if (need <= buf->cap)
		return 0;
	new_cap = buf->cap ? buf->cap : 128;
	while (new_cap < need) {
		if (new_cap > ((size_t)-1) / 2)
			return -1;
		new_cap *= 2;
	}
	new_data = realloc(buf->data, new_cap);
	if (!new_data)
		return -1;
	buf->data = new_data;
	buf->cap = new_cap;
	return 0;
}

static int buf_append_n(struct buf *buf, const char *s, size_t n)
{
	if (buf_reserve(buf, buf->len + n + 1) != 0)
		return -1;
	memcpy(buf->data + buf->len, s, n);
	buf->len += n;
	buf->data[buf->len] = '\0';
	return 0;
}

static int buf_append_c(struct buf *buf, char c)
{
	if (buf_reserve(buf, buf->len + 2) != 0)
		return -1;
	buf->data[buf->len++] = c;
	buf->data[buf->len] = '\0';
	return 0;
}

static int buf_append_str(struct buf *buf, const char *s)
{
	return buf_append_n(buf, s, strlen(s));
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

static int json_get_string(const char *json, const char *key, char **out)
{
	const char *p;
	struct buf buf;

	*out = NULL;
	p = find_key(json, key);
	if (!p)
		return 0;
	if (*p != '"')
		return -1;
	p++;
	if (buf_init(&buf, 128) != 0)
		return -1;
	while (*p && *p != '"') {
		if ((unsigned char)*p < 0x20) {
			buf_free(&buf);
			return -1;
		}
		if (*p == '\\') {
			p++;
			switch (*p) {
			case '"':
			case '\\':
			case '/':
				if (buf_append_c(&buf, *p) != 0)
					goto oom;
				break;
			case 'b':
				if (buf_append_c(&buf, '\b') != 0)
					goto oom;
				break;
			case 'f':
				if (buf_append_c(&buf, '\f') != 0)
					goto oom;
				break;
			case 'n':
				if (buf_append_c(&buf, '\n') != 0)
					goto oom;
				break;
			case 'r':
				if (buf_append_c(&buf, '\r') != 0)
					goto oom;
				break;
			case 't':
				if (buf_append_c(&buf, '\t') != 0)
					goto oom;
				break;
			case 'u':
				if (!isxdigit((unsigned char)p[1]) ||
				    !isxdigit((unsigned char)p[2]) ||
				    !isxdigit((unsigned char)p[3]) ||
				    !isxdigit((unsigned char)p[4])) {
					buf_free(&buf);
					return -1;
				}
				if (buf_append_c(&buf, '?') != 0)
					goto oom;
				p += 4;
				break;
			default:
				buf_free(&buf);
				return -1;
			}
			p++;
		} else {
			if (buf_append_c(&buf, *p++) != 0)
				goto oom;
		}
	}
	if (*p != '"') {
		buf_free(&buf);
		return -1;
	}
	*out = buf.data;
	return 1;

oom:
	buf_free(&buf);
	return -1;
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
		return -1;
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
	return -1;
}

static int parse_args(const char *json, struct read_args *args)
{
	long long max_bytes;
	int rc;

	memset(args, 0, sizeof(*args));
	args->max_bytes = DEFAULT_MAX_BYTES;

	rc = json_get_string(json, "path", &args->path);
	if (rc <= 0)
		return -1;
	rc = json_get_int(json, "start_line", &args->start_line);
	if (rc <= 0)
		return -1;
	rc = json_get_int(json, "line_count", &args->line_count);
	if (rc <= 0)
		return -1;
	rc = json_get_bool(json, "include_line_numbers",
			   &args->include_line_numbers);
	if (rc < 0)
		return -1;
	rc = json_get_int(json, "max_bytes", &max_bytes);
	if (rc < 0)
		return -1;
	if (rc > 0) {
		if (max_bytes <= 0)
			return -1;
		if ((unsigned long long)max_bytes > HARD_MAX_BYTES)
			return -1;
		args->max_bytes = (size_t)max_bytes;
	}

	if (args->start_line <= 0 || args->line_count <= 0 ||
	    args->line_count > MAX_LINE_COUNT)
		return -1;
	if (args->start_line > LLONG_MAX - args->line_count)
		return -1;
	return 0;
}

static void free_args(struct read_args *args)
{
	free(args->path);
	memset(args, 0, sizeof(*args));
}

static int append_json_string(struct buf *buf, const char *s)
{
	const unsigned char *p;
	char tmp[7];

	if (buf_append_c(buf, '"') != 0)
		return -1;
	for (p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"':
			if (buf_append_str(buf, "\\\"") != 0)
				return -1;
			break;
		case '\\':
			if (buf_append_str(buf, "\\\\") != 0)
				return -1;
			break;
		case '\b':
			if (buf_append_str(buf, "\\b") != 0)
				return -1;
			break;
		case '\f':
			if (buf_append_str(buf, "\\f") != 0)
				return -1;
			break;
		case '\n':
			if (buf_append_str(buf, "\\n") != 0)
				return -1;
			break;
		case '\r':
			if (buf_append_str(buf, "\\r") != 0)
				return -1;
			break;
		case '\t':
			if (buf_append_str(buf, "\\t") != 0)
				return -1;
			break;
		default:
			if (*p < 0x20) {
				snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
				if (buf_append_str(buf, tmp) != 0)
					return -1;
			} else if (buf_append_c(buf, (char)*p) != 0) {
				return -1;
			}
			break;
		}
	}
	return buf_append_c(buf, '"');
}

static char *make_error(const char *message)
{
	struct buf out;

	if (buf_init(&out, 128) != 0)
		return xstrdup("{\"error\":\"out of memory\"}");
	if (buf_append_str(&out, "{\"error\":") != 0 ||
	    append_json_string(&out, message) != 0 ||
	    buf_append_c(&out, '}') != 0) {
		buf_free(&out);
		return xstrdup("{\"error\":\"out of memory\"}");
	}
	return out.data;
}

static int path_has_boundary(const char *path, size_t root_len)
{
	return path[root_len] == '\0' || path[root_len] == '/';
}

static int path_under_root(const char *path, const char *root)
{
	size_t root_len;

	root_len = strlen(root);
	if (root_len == 0)
		return 0;
	while (root_len > 1 && root[root_len - 1] == '/')
		root_len--;
	return strncmp(path, root, root_len) == 0 &&
	       path_has_boundary(path, root_len);
}

static int allowed_by_roots_env(const char *real_path, const char *roots)
{
	const char *p;

	p = roots;
	while (*p) {
		const char *end;
		size_t len;
		char raw[PATH_MAX];
		char resolved[PATH_MAX];

		end = strchr(p, ':');
		len = end ? (size_t)(end - p) : strlen(p);
		if (len > 0 && len < sizeof(raw)) {
			memcpy(raw, p, len);
			raw[len] = '\0';
			if (realpath(raw, resolved) &&
			    path_under_root(real_path, resolved))
				return 1;
		}
		if (!end)
			break;
		p = end + 1;
	}
	return 0;
}

static int append_home_root(char *dst, size_t dst_size, const char *home,
			    const char *name)
{
	int n;

	n = snprintf(dst, dst_size, "%s/%s", home, name);
	return n > 0 && (size_t)n < dst_size;
}

static int path_allowed(const char *real_path)
{
	const char *roots;
	char cwd[PATH_MAX];
	const char *home;
	char home_root[PATH_MAX];
	char resolved[PATH_MAX];

	roots = getenv("MORPH_READ_LINES_ROOTS");
	if (roots && roots[0])
		return allowed_by_roots_env(real_path, roots);

	if (getcwd(cwd, sizeof(cwd)) && path_under_root(real_path, cwd))
		return 1;

	home = getenv("HOME");
	if (!home || !home[0])
		return 0;
	if (append_home_root(home_root, sizeof(home_root), home, ".morph") &&
	    realpath(home_root, resolved) && path_under_root(real_path, resolved))
		return 1;
	if (append_home_root(home_root, sizeof(home_root), home, ".agents") &&
	    realpath(home_root, resolved) && path_under_root(real_path, resolved))
		return 1;
	return 0;
}

static int skip_line(FILE *f)
{
	int c;

	while ((c = fgetc(f)) != EOF) {
		if (c == '\0')
			return -2;
		if (c == '\n')
			return 1;
	}
	if (ferror(f))
		return -1;
	return 0;
}

static int read_line_capped(FILE *f, struct buf *line, size_t cap,
			    int *overflow)
{
	int c;
	int saw_any;

	line->len = 0;
	line->data[0] = '\0';
	*overflow = 0;
	saw_any = 0;
	while ((c = fgetc(f)) != EOF) {
		saw_any = 1;
		if (c == '\0')
			return -2;
		if (line->len + 1 <= cap) {
			if (buf_append_c(line, (char)c) != 0)
				return -1;
		} else {
			*overflow = 1;
		}
		if (c == '\n')
			return 1;
	}
	if (ferror(f))
		return -1;
	return saw_any ? 1 : 0;
}

static int append_line_prefix(struct buf *content, long long line_no)
{
	char prefix[64];
	int n;

	n = snprintf(prefix, sizeof(prefix), "%lld: ", line_no);
	if (n <= 0 || (size_t)n >= sizeof(prefix))
		return -1;
	return buf_append_n(content, prefix, (size_t)n);
}

static int read_lines(FILE *f, const struct read_args *args,
		      struct buf *content, long long *end_line,
		      long long *next_line, int *eof, int *truncated)
{
	long long line_no;
	long long i;
	struct buf line;
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

	if (buf_init(&line, args->max_bytes + 1) != 0)
		return -1;

	for (i = 0; i < args->line_count; i++, line_no++) {
		int overflow;
		size_t prefix_len;
		size_t needed;

		rc = read_line_capped(f, &line, args->max_bytes, &overflow);
		if (rc < 0) {
			buf_free(&line);
			return rc;
		}
		if (rc == 0) {
			*eof = 1;
			*next_line = 0;
			break;
		}

		prefix_len = 0;
		if (args->include_line_numbers) {
			char prefix[64];
			int n = snprintf(prefix, sizeof(prefix), "%lld: ",
					 line_no);
			if (n <= 0 || (size_t)n >= sizeof(prefix)) {
				buf_free(&line);
				return -1;
			}
			prefix_len = (size_t)n;
		}

		needed = prefix_len + line.len;
		if (overflow || content->len + needed > args->max_bytes) {
			*truncated = 1;
			*next_line = line_no;
			buf_free(&line);
			return 0;
		}

		if (args->include_line_numbers &&
		    append_line_prefix(content, line_no) != 0) {
			buf_free(&line);
			return -1;
		}
		if (buf_append_n(content, line.data, line.len) != 0) {
			buf_free(&line);
			return -1;
		}
		*end_line = line_no;
		*next_line = line_no + 1;
	}

	if (!*eof && i >= args->line_count)
		*next_line = line_no;
	buf_free(&line);
	return 0;
}

static char *make_result(const char *path, const struct read_args *args,
			 long long end_line, long long next_line,
			 int eof, int truncated, const char *content)
{
	struct buf out;
	char num[128];

	if (buf_init(&out, 1024) != 0)
		return make_error("out of memory");
	if (buf_append_str(&out, "{\"path\":") != 0 ||
	    append_json_string(&out, path) != 0 ||
	    buf_append_str(&out, ",\"start_line\":") != 0)
		goto oom;
	snprintf(num, sizeof(num), "%lld", args->start_line);
	if (buf_append_str(&out, num) != 0 ||
	    buf_append_str(&out, ",\"line_count\":") != 0)
		goto oom;
	snprintf(num, sizeof(num), "%lld", args->line_count);
	if (buf_append_str(&out, num) != 0 ||
	    buf_append_str(&out, ",\"end_line\":") != 0)
		goto oom;
	snprintf(num, sizeof(num), "%lld", end_line);
	if (buf_append_str(&out, num) != 0 ||
	    buf_append_str(&out, ",\"next_line\":") != 0)
		goto oom;
	if (next_line > 0) {
		snprintf(num, sizeof(num), "%lld", next_line);
		if (buf_append_str(&out, num) != 0)
			goto oom;
	} else if (buf_append_str(&out, "null") != 0) {
		goto oom;
	}
	if (buf_append_str(&out, ",\"eof\":") != 0 ||
	    buf_append_str(&out, eof ? "true" : "false") != 0 ||
	    buf_append_str(&out, ",\"truncated\":") != 0 ||
	    buf_append_str(&out, truncated ? "true" : "false") != 0 ||
	    buf_append_str(&out, ",\"content\":") != 0 ||
	    append_json_string(&out, content) != 0 ||
	    buf_append_c(&out, '}') != 0)
		goto oom;
	return out.data;

oom:
	buf_free(&out);
	return make_error("out of memory");
}

int ext_run(const char *args_json, char **result_json)
{
	struct read_args args;
	char real_path[PATH_MAX];
	struct stat st;
	FILE *f;
	struct buf content;
	long long end_line;
	long long next_line;
	int eof;
	int truncated;
	int rc;

	if (!args_json || !result_json)
		return -1;
	*result_json = NULL;
	if (parse_args(args_json, &args) != 0) {
		*result_json = make_error("invalid arguments");
		return -1;
	}

	if (args.path[0] != '/') {
		*result_json = make_error("path must be absolute");
		free_args(&args);
		return -1;
	}
	if (!realpath(args.path, real_path)) {
		*result_json = make_error("file not found");
		free_args(&args);
		return -1;
	}
	if (!path_allowed(real_path)) {
		*result_json = make_error("path is not under an allowed root");
		free_args(&args);
		return -1;
	}
	if (stat(real_path, &st) != 0 || !S_ISREG(st.st_mode)) {
		*result_json = make_error("path is not a regular file");
		free_args(&args);
		return -1;
	}

	f = fopen(real_path, "rb");
	if (!f) {
		*result_json = make_error("failed to open file");
		free_args(&args);
		return -1;
	}

	if (buf_init(&content, args.max_bytes + 1) != 0) {
		fclose(f);
		free_args(&args);
		*result_json = make_error("out of memory");
		return -1;
	}

	rc = read_lines(f, &args, &content, &end_line, &next_line, &eof,
			&truncated);
	fclose(f);
	if (rc == -2) {
		*result_json = make_error("file appears to be binary");
		buf_free(&content);
		free_args(&args);
		return -1;
	}
	if (rc != 0) {
		*result_json = make_error("failed to read file");
		buf_free(&content);
		free_args(&args);
		return -1;
	}

	*result_json = make_result(real_path, &args, end_line, next_line, eof,
				   truncated, content.data);
	buf_free(&content);
	free_args(&args);
	return *result_json ? 0 : -1;
}
