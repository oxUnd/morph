#include "file_read.h"
#include "agent/tool_context.h"
#include "agent/tool_runtime.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/utf8.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FILE_READ_DEFAULT_BYTES (50 * 1024)
#define FILE_READ_MAX_BYTES (256 * 1024)
#define FILE_READ_LINE_BYTES (16 * 1024)
#define FILE_READ_MAX_INTEGER UINT64_C(9007199254740991)

struct read_options {
	const char *path;
	const char *expected_version;
	uint64_t offset;
	uint64_t byte_offset;
	uint64_t limit;
	uint64_t max_bytes;
	int byte_mode;
};

struct file_reader {
	int fd;
	const struct http_control *control;
	unsigned char chunk[BUFSIZ + 4];
	size_t begin;
	size_t end;
	uint64_t position;
	uint64_t size;
};

struct read_page {
	morph_buf_t content;
	uint64_t line;
	uint64_t returned;
	uint64_t line_bytes;
	int known_lines;
	int starts_mid_line;
	int binary;
	const char *encoding;
	const char *reason;
};

static int read_integer(cJSON *root, const char *name, uint64_t min,
			uint64_t max, uint64_t *value)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

	if (!item)
		return 0;
	if (!cJSON_IsNumber(item) || !(item->valuedouble >= (double)min) ||
	    !(item->valuedouble <= (double)max))
		return -EINVAL;
	uint64_t parsed = (uint64_t)item->valuedouble;
	if ((double)parsed != item->valuedouble)
		return -EINVAL;
	*value = parsed;
	return 0;
}

static int read_options_parse(cJSON *root, struct read_options *opts)
{
	cJSON *path = cJSON_GetObjectItemCaseSensitive(root, "file_path");
	cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "expected_version");

	if (!cJSON_IsObject(root) || !cJSON_IsString(path) ||
	    !path->valuestring[0] || utf8valid(path->valuestring) ||
	    (version && !cJSON_IsString(version)))
		return -EINVAL;
	opts->path = path->valuestring;
	opts->expected_version = version ? version->valuestring : NULL;
	opts->limit = 1000;
	opts->max_bytes = FILE_READ_DEFAULT_BYTES;
	opts->byte_mode = cJSON_HasObjectItem(root, "byte_offset");
	if (opts->byte_mode && cJSON_HasObjectItem(root, "offset"))
		return -EINVAL;
	if (read_integer(root, "offset", 0, FILE_READ_MAX_INTEGER, &opts->offset) < 0 ||
	    read_integer(root, "byte_offset", 0, FILE_READ_MAX_INTEGER,
			 &opts->byte_offset) < 0 ||
	    read_integer(root, "limit", 1, 1000000, &opts->limit) < 0 ||
	    read_integer(root, "max_lines", 1, 1000000, &opts->limit) < 0 ||
	    read_integer(root, "max_bytes", 4, FILE_READ_MAX_BYTES, &opts->max_bytes) < 0)
		return -EINVAL;
	return 0;
}

/* Keep read-ahead fixed, including when a single source line is enormous. */
static int reader_fill(struct file_reader *reader)
{
	size_t available = reader->end - reader->begin;
	if (available >= 4 || reader->position + available == reader->size)
		return 0;
	int rc = http_wait_cancelable(reader->control, 0);
	if (rc < 0)
		return rc;
	memmove(reader->chunk, reader->chunk + reader->begin, available);
	reader->begin = 0;
	reader->end = available;
	while (reader->end < 4 && reader->position + reader->end < reader->size) {
		size_t count = sizeof(reader->chunk) - reader->end;
		uint64_t remaining = reader->size - reader->position - reader->end;
		if (remaining < count)
			count = (size_t)remaining;
		ssize_t n = read(reader->fd, reader->chunk + reader->end, count);
		if (n < 0) {
			if (errno == EINTR) {
				rc = http_wait_cancelable(reader->control, 0);
				if (rc < 0)
					return rc;
				continue;
			}
			return -errno;
		}
		if (n == 0)
			return -ESTALE;
		reader->end += (size_t)n;
	}
	return 0;
}

static void reader_consume(struct file_reader *reader, size_t count)
{
	reader->begin += count;
	reader->position += count;
}

static int read_version(const struct stat *st, morph_buf_t *version)
{
#ifdef __APPLE__
	const struct timespec modified = st->st_mtimespec;
	const struct timespec changed = st->st_ctimespec;
#else
	const struct timespec modified = st->st_mtim;
	const struct timespec changed = st->st_ctim;
#endif
	morph_buf_reset(version);
	return morph_buf_printf(version, "%ju:%ju:%jd:%jd:%ld:%jd:%ld",
		(uintmax_t)st->st_dev, (uintmax_t)st->st_ino,
		(intmax_t)st->st_size, (intmax_t)modified.tv_sec, modified.tv_nsec,
		(intmax_t)changed.tv_sec, changed.tv_nsec);
}

static int reader_start(struct file_reader *reader, const struct read_options *opts,
			struct read_page *page)
{
	int rc = reader_fill(reader);
	if (rc < 0)
		return rc;
	const unsigned char *p = reader->chunk;
	size_t n = reader->end;

	page->encoding = "utf-8";
	if ((n >= 4 && !memcmp(p, "\x00\x00\xfe\xff", 4)) ||
	    (n >= 4 && !memcmp(p, "\xff\xfe\x00\x00", 4))) {
		page->encoding = "utf-32";
		page->binary = 1;
	} else if ((n >= 2 && !memcmp(p, "\xff\xfe", 2)) ||
		   (n >= 2 && !memcmp(p, "\xfe\xff", 2))) {
		page->encoding = "utf-16";
		page->binary = 1;
	}
	if (page->binary)
		return 0;
	if (!opts->byte_mode || opts->byte_offset == 0) {
		page->known_lines = 1;
		if (n >= 3 && !memcmp(p, "\xef\xbb\xbf", 3))
			reader_consume(reader, 3);
		return 0;
	}
	if (opts->byte_offset > reader->size)
		return -EINVAL;
	unsigned char previous;
	ssize_t count = pread(reader->fd, &previous, 1, (off_t)opts->byte_offset - 1);
	if (count < 0)
		return -errno;
	if (count != 1)
		return -ESTALE;
	page->starts_mid_line = previous != '\n';
	if (opts->byte_offset == 3 && n >= 3 && !memcmp(p, "\xef\xbb\xbf", 3))
		page->starts_mid_line = 0;
	if (lseek(reader->fd, (off_t)opts->byte_offset, SEEK_SET) < 0)
		return -errno;
	reader->begin = 0;
	reader->end = 0;
	reader->position = opts->byte_offset;
	return 0;
}

static int reader_skip_lines(struct file_reader *reader, uint64_t offset,
			     struct read_page *page)
{
	int partial = 0;

	while (page->line < offset && reader->position < reader->size) {
		int rc = reader_fill(reader);
		if (rc < 0)
			return rc;
		const unsigned char *p = reader->chunk + reader->begin;
		size_t available = reader->end - reader->begin;
		const unsigned char *newline = memchr(p, '\n', available);
		size_t count = newline ? (size_t)(newline - p) + 1 : available;
		reader_consume(reader, count);
		partial = newline == NULL;
		if (newline)
			page->line++;
	}
	if (reader->position == reader->size && partial)
		page->line++;
	return 0;
}

static int reader_text(struct file_reader *reader, const struct read_options *opts,
			struct read_page *page)
{
	while (reader->position < reader->size) {
		if (page->returned >= opts->limit) {
			page->reason = "line_limit";
			break;
		}
		if (page->content.len >= opts->max_bytes) {
			page->reason = "max_bytes";
			break;
		}
		int rc = reader_fill(reader);
		if (rc < 0)
			return rc;
		const char *p = (const char *)reader->chunk + reader->begin;
		unsigned cp;
		size_t count;
		if (*p == '\0') {
			page->binary = 1;
			page->encoding = "binary";
			break;
		}
		if (!utf8_decode_codepoint(p, reader->end - reader->begin, &cp, &count)) {
			/* A caller's cursor must point to a complete UTF-8 character. */
			if (opts->byte_mode && reader->position == opts->byte_offset &&
			    utf8_is_continuation_byte((unsigned char)*p))
				return -EINVAL;
			page->binary = 1;
			page->encoding = "unknown";
			break;
		}
		if (page->content.len + count > opts->max_bytes) {
			page->reason = "max_bytes";
			break;
		}
		if (cp != '\n' && page->line_bytes + count > FILE_READ_LINE_BYTES) {
			page->reason = "line_length";
			break;
		}
		rc = morph_buf_append(&page->content, p, count);
		if (rc < 0)
			return rc;
		reader_consume(reader, count);
		page->line_bytes += count;
		if (cp == '\n') {
			page->line++;
			page->returned++;
			page->line_bytes = 0;
		}
	}
	if (page->line_bytes)
		page->returned++;
	return 0;
}

static int read_binary_result(struct file_reader *reader, cJSON *out)
{
	unsigned char bytes[64];
	char hex[sizeof(bytes) * 3 + 1];
	ssize_t n = pread(reader->fd, bytes, sizeof(bytes), 0);

	if (n < 0)
		return -errno;
	for (size_t i = 0; i < (size_t)n; i++) {
		hex[i * 3] = "0123456789abcdef"[bytes[i] >> 4];
		hex[i * 3 + 1] = "0123456789abcdef"[bytes[i] & 15];
		hex[i * 3 + 2] = ' ';
	}
	hex[n > 0 ? (size_t)n * 3 - 1 : 0] = '\0';
	if (!cJSON_AddStringToObject(out, "preview", hex) ||
	    !cJSON_AddStringToObject(out, "preview_format", "hex") ||
	    !cJSON_AddStringToObject(out, "note",
		"Binary or unsupported text encoding; content was not returned. "
		"Convert UTF-16/32 or other encodings to UTF-8 before reading.") ||
	    !cJSON_AddNumberToObject(out, "returned_lines", 0) ||
	    !cJSON_AddNullToObject(out, "total_lines") ||
	    !cJSON_AddBoolToObject(out, "truncated", 1))
		return -ENOMEM;
	return 0;
}

static cJSON *read_optional_number(cJSON *out, const char *name, int known, uint64_t value)
{
	return known ? cJSON_AddNumberToObject(out, name, (double)value) :
		cJSON_AddNullToObject(out, name);
}

static int read_text_result(struct file_reader *reader, const struct read_options *opts,
			    struct read_page *page, cJSON *out)
{
	int more = reader->position < reader->size;
	int partial = more && page->line_bytes != 0;
	uint64_t total = page->line + (page->line_bytes != 0);
	uint64_t start = opts->byte_mode ? 0 : opts->offset;

	if (!cJSON_AddNumberToObject(out, "returned_lines", (double)page->returned) ||
	    !cJSON_AddBoolToObject(out, "truncated", more) ||
	    !cJSON_AddBoolToObject(out, "partial_line", partial) ||
	    !cJSON_AddBoolToObject(out, "starts_mid_line", page->starts_mid_line) ||
	    !read_optional_number(out, "total_lines", !more && page->known_lines, total) ||
	    !read_optional_number(out, "line_start", page->known_lines,
			  start < total ? start : total) ||
	    !read_optional_number(out, "line_end", page->known_lines,
			  page->line + (page->line_bytes != 0)) ||
	    !read_optional_number(out, "next_offset", more && !partial && page->known_lines,
			  page->line) ||
	    !read_optional_number(out, "next_byte_offset", more, reader->position) ||
	    !cJSON_AddStringToObject(out, "truncation_reason", page->reason ? page->reason : ""))
		return -ENOMEM;
	if (more && !cJSON_AddStringToObject(out, "note",
		"More content remains. Continue with byte_offset=next_byte_offset and "
		"expected_version=version; omit offset. Line ranges are 0-based, end-exclusive. "
		"A partial_line continues on the next page without dropping bytes."))
		return -ENOMEM;
	if (!cJSON_AddStringToObject(out, "content", morph_buf_cstr(&page->content)))
		return -ENOMEM;
	return 0;
}

static int read_open_file(struct file_reader *reader, const struct read_options *opts,
			  struct tool_result *result)
{
	struct stat before;
	struct stat after;
	morph_buf_t version = {0};
	morph_buf_t after_version = {0};
	struct read_page page = {0};
	cJSON *out = NULL;
	int rc;

	if (fstat(reader->fd, &before) < 0)
		return -errno;
	if (!S_ISREG(before.st_mode))
		return S_ISDIR(before.st_mode) ? -EISDIR : -ENOTSUP;
	if (before.st_size < 0 || (uintmax_t)before.st_size > FILE_READ_MAX_INTEGER)
		return -EOVERFLOW;
	reader->size = (uint64_t)before.st_size;
	if (opts->byte_offset > reader->size)
		return -EINVAL;
	rc = (morph_buf_init(&version, 128));
	if (rc < 0)
		return rc;
	rc = (read_version(&before, &version));
	if (rc < 0)
		goto out_cleanup;
	if (opts->expected_version && strcmp(opts->expected_version, morph_buf_cstr(&version))) {
		rc = (-ESTALE);
		goto out_cleanup;
	}
	rc = (morph_buf_init(&page.content, (size_t)opts->max_bytes + 1));
	if (rc < 0)
		goto out_cleanup;
	rc = (reader_start(reader, opts, &page));
	if (rc < 0)
		goto out_cleanup;
	if (!page.binary && !opts->byte_mode) {
		rc = (reader_skip_lines(reader, opts->offset, &page));
		if (rc < 0)
			goto out_cleanup;
	}
	if (!page.binary) {
		rc = (reader_text(reader, opts, &page));
		if (rc < 0)
			goto out_cleanup;
	}
	out = cJSON_CreateObject();
	if (!out || !cJSON_AddStringToObject(out, "file_path", opts->path) ||
	    !cJSON_AddNumberToObject(out, "size", (double)reader->size) ||
	    !cJSON_AddBoolToObject(out, "binary", page.binary) ||
	    !cJSON_AddStringToObject(out, "encoding", page.encoding) ||
	    !cJSON_AddStringToObject(out, "version", morph_buf_cstr(&version))) {
		rc = (-ENOMEM);
		goto out_cleanup;
	}
	rc = (page.binary ? read_binary_result(reader, out) :
		      read_text_result(reader, opts, &page, out));
	if (rc < 0)
		goto out_cleanup;
	if (fstat(reader->fd, &after) < 0) {
		rc = (-errno);
		goto out_cleanup;
	}
	rc = (morph_buf_init(&after_version, 128));
	if (rc < 0)
		goto out_cleanup;
	rc = (read_version(&after, &after_version));
	if (rc < 0)
		goto out_cleanup;
	if (strcmp(morph_buf_cstr(&version), morph_buf_cstr(&after_version))) {
		rc = (-ESTALE);
		goto out_cleanup;
	}
	rc = (tool_result_success(result, out));
	out = NULL;
out_cleanup:
	cJSON_Delete(out);
	morph_buf_cleanup(&after_version);
	morph_buf_cleanup(&page.content);
	morph_buf_cleanup(&version);
	return rc;
}

static int file_read_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct tool_context *tctx = user_data;
	struct read_options opts = {0};
	char resolved[PATH_MAX];
	int rc;

	if (!result)
		return -EINVAL;
	cJSON *root = args_json ? cJSON_Parse(args_json) : NULL;
	rc = read_options_parse(root, &opts);
	if (rc < 0) {
		cJSON_Delete(root);
		(void)tool_result_error(result, "invalid_arguments",
			"Provide file_path, nonnegative integer offset OR byte_offset, "
			"positive limit/max_lines (up to 1000000), and max_bytes (4..262144).");
		return rc;
	}
	if (tctx)
		rc = tool_context_authorize_path(tctx, TOOL_PATH_READ, opts.path,
					 resolved, sizeof(resolved));
	else if (strlen(opts.path) >= sizeof(resolved))
		rc = (-ENAMETOOLONG);
	else
		memcpy(resolved, opts.path, strlen(opts.path) + 1);
	struct file_reader reader = {
		.fd = -1,
		.control = result->runtime ? &result->runtime->http : NULL,
	};
	if (rc == 0)
		rc = http_wait_cancelable(reader.control, 0);
	if (rc == 0) {
		reader.fd = open(resolved, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
		if (reader.fd < 0)
			rc = (-errno);
	}
	if (rc == 0)
		rc = read_open_file(&reader, &opts, result);
	if (reader.fd >= 0)
		close(reader.fd);
	cJSON_Delete(root);
	if (rc < 0) {
		const char *message = rc == -ESTALE ?
			"File changed; restart reading to obtain a new version." :
			rc == -EPERM ? "read path outside workspace: permission denied" :
			morph_strerror(rc);
		(void)tool_result_error(result, "file_read_failed", message);
	}
	return rc;
}

int file_read_init(struct tool_registry *reg, struct tool_context *tctx)
{
	if (!reg)
		return -EINVAL;
	return tool_register(reg, &(struct tool_spec){
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "file_read",
		.description = "Read UTF-8 text verbatim with bounded memory. "
			"offset is a 0-based line index; limit/max_lines defaults to 1000. "
			"max_bytes defaults to 51200 (4..262144). Lines over 16384 bytes "
			"return lossless fragments. Continue using byte_offset=next_byte_offset "
			"and expected_version=version, omitting offset. total_lines is null "
			"until EOF when known; line_start/line_end are 0-based, end-exclusive. "
			"Binary/unsupported encodings return a hex preview, not text.",
		.input_schema = "{\"type\":\"object\",\"properties\":{"
			"\"file_path\":{\"type\":\"string\"},"
			"\"offset\":{\"type\":\"integer\",\"minimum\":0},"
			"\"byte_offset\":{\"type\":\"integer\",\"minimum\":0},"
			"\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000000},"
			"\"max_lines\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000000},"
			"\"max_bytes\":{\"type\":\"integer\",\"minimum\":4,\"maximum\":262144},"
			"\"expected_version\":{\"type\":\"string\"}},\"required\":[\"file_path\"]}",
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = file_read_exec,
		.user_data = tctx,
	});
}
