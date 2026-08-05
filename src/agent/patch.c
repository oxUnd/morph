#include "patch.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/file.h"
#include "util/utf8.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATCH_MAX_BYTES (64 * 1024)

struct patch_chunk {
	char *context;
	morph_array_t old_lines;
	morph_array_t new_lines;
	int end_of_file;
};

struct patch_hunk {
	enum patch_action action;
	char *path;
	char *move_path;
	char *add_content;
	morph_array_t chunks;
	int added;
	int removed;
};

struct patch_replacement {
	size_t start;
	size_t old_count;
	size_t order;
	struct patch_chunk *chunk;
	size_t new_count;
};

enum patch_header {
	PATCH_HEADER_NONE,
	PATCH_HEADER_ADD,
	PATCH_HEADER_UPDATE,
	PATCH_HEADER_DELETE,
	PATCH_HEADER_MOVE,
	PATCH_HEADER_BEGIN,
	PATCH_HEADER_END,
	PATCH_HEADER_EOF,
	PATCH_HEADER_ENVIRONMENT,
};

static int patch_error(char *error, size_t error_size, int code,
		       const char *fmt, ...)
{
	va_list ap;

	if (error && error_size > 0) {
		va_start(ap, fmt);
		(void)vsnprintf(error, error_size, fmt, ap);
		va_end(ap);
	}
	return code;
}

static void patch_trim_view(const char *text, const char **begin, size_t *len)
{
	const char *start = text;
	size_t size = strlen(text);

	while (size > 0 && (start[0] == ' ' || start[0] == '\t' ||
			    start[0] == '\r')) {
		start++;
		size--;
	}
	while (size > 0 && (start[size - 1] == ' ' ||
			    start[size - 1] == '\t' ||
			    start[size - 1] == '\r'))
		size--;
	*begin = start;
	*len = size;
}

static int patch_view_equal(const char *line, const char *marker)
{
	const char *begin;
	size_t len;

	patch_trim_view(line, &begin, &len);
	return strlen(marker) == len && memcmp(begin, marker, len) == 0;
}

static enum patch_header patch_header_kind(const char *line,
					   const char **value,
					   size_t *value_len)
{
	static const struct {
		const char *marker;
		enum patch_header header;
		int has_value;
	} markers[] = {
		{"*** Add File: ", PATCH_HEADER_ADD, 1},
		{"*** Update File: ", PATCH_HEADER_UPDATE, 1},
		{"*** Delete File: ", PATCH_HEADER_DELETE, 1},
		{"*** Move to: ", PATCH_HEADER_MOVE, 1},
		{"*** Environment ID: ", PATCH_HEADER_ENVIRONMENT, 1},
		{"*** Begin Patch", PATCH_HEADER_BEGIN, 0},
		{"*** End Patch", PATCH_HEADER_END, 0},
		{"*** End of File", PATCH_HEADER_EOF, 0},
	};
	const char *begin;
	size_t len;

	patch_trim_view(line, &begin, &len);
	for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
		size_t marker_len = strlen(markers[i].marker);

		if ((!markers[i].has_value && len == marker_len) ||
		    (markers[i].has_value && len > marker_len)) {
			if (memcmp(begin, markers[i].marker, marker_len) != 0)
				continue;
			if (begin == line + 1 && line[0] == ' ' &&
			    (markers[i].header == PATCH_HEADER_ADD ||
			     markers[i].header == PATCH_HEADER_UPDATE ||
			     markers[i].header == PATCH_HEADER_DELETE ||
			     markers[i].header == PATCH_HEADER_MOVE))
				return PATCH_HEADER_NONE;
			if (value)
				*value = begin + marker_len;
			if (value_len)
				*value_len = len - marker_len;
			return markers[i].header;
		}
	}
	return PATCH_HEADER_NONE;
}

static int patch_string_array_init(morph_array_t *array)
{
	memset(array, 0, sizeof(*array));
	return morph_array_init(array, 4, sizeof(char *));
}

static void patch_string_array_cleanup(morph_array_t *array)
{
	char **value;

	if (!array)
		return;
	morph_array_foreach(value, array, char *)
		free(*value);
	morph_array_cleanup(array);
}

static int patch_string_array_push(morph_array_t *array, const char *text,
				   size_t len)
{
	char **slot = morph_array_push(array);

	if (!slot)
		MORPH_RETURN(-ENOMEM);
	*slot = strndup(text, len);
	if (!*slot) {
		array->nelts--;
		MORPH_RETURN(-ENOMEM);
	}
	return 0;
}

static void patch_chunk_cleanup(struct patch_chunk *chunk)
{
	if (!chunk)
		return;
	free(chunk->context);
	patch_string_array_cleanup(&chunk->old_lines);
	patch_string_array_cleanup(&chunk->new_lines);
	memset(chunk, 0, sizeof(*chunk));
}

static void patch_hunks_cleanup(morph_array_t *hunks)
{
	struct patch_hunk *hunk;

	if (!hunks)
		return;
	morph_array_foreach(hunk, hunks, struct patch_hunk) {
		free(hunk->path);
		free(hunk->move_path);
		free(hunk->add_content);
		for (size_t chunk_index = 0;
		     chunk_index < hunk->chunks.nelts; chunk_index++) {
			struct patch_chunk *chunk = morph_array_get(&hunk->chunks,
				chunk_index);

			patch_chunk_cleanup(chunk);
		}
		morph_array_cleanup(&hunk->chunks);
	}
	morph_array_cleanup(hunks);
}

static int patch_split_lines(char *copy, morph_array_t *lines)
{
	char *start = copy;
	char *cursor = copy;

	while (1) {
		if (*cursor == '\r' && cursor[1] == '\n') {
			char **slot;

			*cursor = '\0';
			slot = morph_array_push(lines);
			if (!slot)
				MORPH_RETURN(-ENOMEM);
			*slot = start;
			cursor += 2;
			start = cursor;
			continue;
		}
		if (*cursor == '\n' || *cursor == '\0') {
			char **slot;
			int done = *cursor == '\0';

			*cursor = '\0';
			slot = morph_array_push(lines);
			if (!slot)
				MORPH_RETURN(-ENOMEM);
			*slot = start;
			if (done)
				break;
			start = cursor + 1;
		}
		cursor++;
	}
	return 0;
}

static int patch_append_add_line(morph_buf_t *content, const char *line)
{
	int rc;

	if (line[0] != '+')
		MORPH_RETURN(-EINVAL);
	rc = morph_buf_puts(content, line + 1);
	if (rc == 0)
		rc = morph_buf_putc(content, '\n');
	return rc;
}

static int patch_chunk_init(struct patch_chunk *chunk)
{
	int rc;

	memset(chunk, 0, sizeof(*chunk));
	rc = patch_string_array_init(&chunk->old_lines);
	if (rc != 0)
		return rc;
	rc = patch_string_array_init(&chunk->new_lines);
	if (rc != 0) {
		patch_string_array_cleanup(&chunk->old_lines);
		return rc;
	}
	return 0;
}

static int patch_parse_change_line(struct patch_hunk *hunk,
				   struct patch_chunk *chunk,
				   const char *line, size_t line_number,
				   char *error, size_t error_size)
{
	char marker = line[0];
	const char *text;
	int rc;

	if (line[0] == '\0') {
		marker = ' ';
		text = line;
	} else if (marker == ' ' || marker == '+' || marker == '-') {
		text = line + 1;
	} else {
		return patch_error(error, error_size, -EINVAL,
			"invalid hunk line %zu in %s", line_number, hunk->path);
	}
	if (marker != '+') {
		rc = patch_string_array_push(&chunk->old_lines, text,
			strlen(text));
		if (rc != 0)
			return rc;
	}
	if (marker != '-') {
		rc = patch_string_array_push(&chunk->new_lines, text,
			strlen(text));
		if (rc != 0)
			return rc;
	}
	if (marker == '+')
		hunk->added++;
	else if (marker == '-')
		hunk->removed++;
	return 0;
}

static int patch_parse_update(char **lines, size_t *index, size_t end,
			      struct patch_hunk *hunk, char *error,
			      size_t error_size)
{
	const char *value;
	size_t value_len;
	enum patch_header header;
	int rc;

	if (*index < end) {
		header = patch_header_kind(lines[*index], &value, &value_len);
		if (header == PATCH_HEADER_MOVE) {
			hunk->move_path = strndup(value, value_len);
			if (!hunk->move_path)
				MORPH_RETURN(-ENOMEM);
			(*index)++;
		}
	}
	while (*index < end) {
		struct patch_chunk *chunk;
		const char *trimmed;
		size_t trimmed_len;

		header = patch_header_kind(lines[*index], NULL, NULL);
		if (header == PATCH_HEADER_ADD || header == PATCH_HEADER_UPDATE ||
		    header == PATCH_HEADER_DELETE || header == PATCH_HEADER_END)
			break;
		chunk = morph_array_push(&hunk->chunks);
		if (!chunk)
			MORPH_RETURN(-ENOMEM);
		rc = patch_chunk_init(chunk);
		if (rc != 0) {
			hunk->chunks.nelts--;
			return rc;
		}
		patch_trim_view(lines[*index], &trimmed, &trimmed_len);
		if (trimmed_len == 2 && memcmp(trimmed, "@@", 2) == 0) {
			(*index)++;
		} else if (trimmed_len > 3 && memcmp(trimmed, "@@ ", 3) == 0) {
			chunk->context = strndup(trimmed + 3, trimmed_len - 3);
			if (!chunk->context)
				return -ENOMEM;
			(*index)++;
		} else if (hunk->chunks.nelts > 1) {
			return patch_error(error, error_size, -EINVAL,
				"expected @@ hunk header at line %zu in %s",
				*index + 1, hunk->path);
		}
		while (*index < end) {
			patch_trim_view(lines[*index], &trimmed, &trimmed_len);
			header = patch_header_kind(lines[*index], NULL, NULL);
			if ((trimmed_len == 2 && memcmp(trimmed, "@@", 2) == 0) ||
			    (trimmed_len > 3 && memcmp(trimmed, "@@ ", 3) == 0) ||
			    header == PATCH_HEADER_ADD ||
			    header == PATCH_HEADER_UPDATE ||
			    header == PATCH_HEADER_DELETE ||
			    header == PATCH_HEADER_END)
				break;
			if (header == PATCH_HEADER_EOF) {
				chunk->end_of_file = 1;
				(*index)++;
				while (*index < end &&
				       patch_view_equal(lines[*index], ""))
					(*index)++;
				break;
			}
			rc = patch_parse_change_line(hunk, chunk, lines[*index],
				*index + 1, error, error_size);
			if (rc != 0)
				return rc;
			(*index)++;
		}
		if (chunk->old_lines.nelts == 0 && chunk->new_lines.nelts == 0)
			return patch_error(error, error_size, -EINVAL,
				"empty update hunk at line %zu in %s", *index + 1,
				hunk->path);
	}
	if (hunk->chunks.nelts == 0)
		return patch_error(error, error_size, -EINVAL,
			"update file hunk for path '%s' is empty", hunk->path);
	return 0;
}

static int patch_parse(char *copy, morph_array_t *hunks, char *error,
		       size_t error_size)
{
	morph_array_t lines;
	char **line;
	size_t first = 0;
	size_t end;
	size_t index;
	int rc;

	memset(&lines, 0, sizeof(lines));
	rc = morph_array_init(&lines, 64, sizeof(char *));
	if (rc != 0)
		return rc;
	rc = patch_split_lines(copy, &lines);
	if (rc != 0) {
		morph_array_cleanup(&lines);
		return rc;
	}
	line = lines.elts;
	end = lines.nelts;
	while (first < end && patch_view_equal(line[first], ""))
		first++;
	while (end > first && patch_view_equal(line[end - 1], ""))
		end--;
	if (end - first >= 4 &&
	    (strcmp(line[first], "<<EOF") == 0 ||
	     strcmp(line[first], "<<'EOF'") == 0 ||
	     strcmp(line[first], "<<\"EOF\"") == 0) &&
	    strcmp(line[end - 1], "EOF") == 0) {
		first++;
		end--;
	}
	if (first >= end ||
	    patch_header_kind(line[first], NULL, NULL) != PATCH_HEADER_BEGIN) {
		rc = patch_error(error, error_size, -EINVAL,
			"patch must start with *** Begin Patch");
		morph_array_cleanup(&lines);
		return rc;
	}
	if (patch_header_kind(line[end - 1], NULL, NULL) != PATCH_HEADER_END) {
		rc = patch_error(error, error_size, -EINVAL,
			"patch must end with *** End Patch");
		morph_array_cleanup(&lines);
		return rc;
	}
	index = first + 1;
	if (index < end - 1 && patch_header_kind(line[index], NULL, NULL) ==
	    PATCH_HEADER_ENVIRONMENT)
		index++;
	while (index < end - 1) {
		struct patch_hunk *hunk;
		const char *value = NULL;
		size_t value_len = 0;
		enum patch_header header = patch_header_kind(line[index], &value,
			&value_len);

		if (header != PATCH_HEADER_ADD && header != PATCH_HEADER_UPDATE &&
		    header != PATCH_HEADER_DELETE) {
			rc = patch_error(error, error_size, -EINVAL,
				"invalid patch hunk on line %zu: %s", index + 1,
				line[index]);
			break;
		}
		hunk = morph_array_push(hunks);
		if (!hunk) {
			rc = -ENOMEM;
			break;
		}
		memset(hunk, 0, sizeof(*hunk));
		hunk->action = header == PATCH_HEADER_ADD ? PATCH_ACTION_ADD :
			header == PATCH_HEADER_UPDATE ? PATCH_ACTION_UPDATE :
			PATCH_ACTION_DELETE;
		hunk->path = strndup(value, value_len);
		if (!hunk->path) {
			rc = -ENOMEM;
			break;
		}
		rc = morph_array_init(&hunk->chunks, 2,
			sizeof(struct patch_chunk));
		if (rc != 0)
			break;
		index++;
		if (hunk->action == PATCH_ACTION_ADD) {
			morph_buf_t content;

			rc = morph_buf_init(&content, 128);
			if (rc != 0)
				break;
			while (index < end - 1 &&
			       patch_header_kind(line[index], NULL, NULL) ==
			       PATCH_HEADER_NONE) {
				rc = patch_append_add_line(&content, line[index]);
				if (rc != 0)
					break;
				hunk->added++;
				index++;
			}
			if (rc == 0 && hunk->added == 0)
				rc = patch_error(error, error_size, -EINVAL,
					"add file has no content lines: %s", hunk->path);
			if (rc == 0)
				hunk->add_content = morph_buf_detach(&content);
			morph_buf_cleanup(&content);
		} else if (hunk->action == PATCH_ACTION_UPDATE) {
			rc = patch_parse_update(line, &index, end - 1, hunk,
				error, error_size);
		}
		if (rc != 0)
			break;
	}
	if (rc == 0 && hunks->nelts == 0)
		rc = patch_error(error, error_size, -EINVAL,
			"No files were modified.");
	morph_array_cleanup(&lines);
	return rc;
}

static int patch_path_valid(const char *path)
{
	const char *part;

	if (!path || !*path || file_path_is_absolute(path))
		return 0;
	part = path;
	while (*part) {
		const char *slash = strchr(part, '/');
		size_t len = slash ? (size_t)(slash - part) : strlen(part);

		if (len == 0 || (len == 1 && part[0] == '.') ||
		    (len == 2 && part[0] == '.' && part[1] == '.'))
			return 0;
		if (!slash)
			break;
		part = slash + 1;
	}
	return 1;
}

static int patch_reject_symlink_components(const char *root, const char *path,
					   char *error,
					   size_t error_size)
{
	char current[PATH_MAX];
	const char *part = path;
	struct stat st;

	if (strlen(root) >= sizeof(current))
		MORPH_RETURN(-ENAMETOOLONG);
	strncpy(current, root, sizeof(current) - 1);
	current[sizeof(current) - 1] = '\0';
	while (*part) {
		const char *slash = strchr(part, '/');
		size_t len = slash ? (size_t)(slash - part) : strlen(part);
		size_t used = strlen(current);

		if (used + len + 2 > sizeof(current))
			MORPH_RETURN(-ENAMETOOLONG);
		current[used++] = '/';
		memcpy(current + used, part, len);
		current[used + len] = '\0';
		if (lstat(current, &st) == 0 && S_ISLNK(st.st_mode))
			return patch_error(error, error_size, -ELOOP,
				"symbolic-link targets are not supported: %s", path);
		if (!slash)
			break;
		part = slash + 1;
	}
	return 0;
}

static int patch_resolve_target(const char *root, const char *path,
				char *resolved, size_t resolved_size,
				char *error, size_t error_size)
{
	int rc;

	if (!patch_path_valid(path))
		return patch_error(error, error_size, -EINVAL,
			"invalid relative path: %s", path ? path : "(null)");
	rc = patch_reject_symlink_components(root, path, error, error_size);
	if (rc != 0)
		return rc;
	rc = file_path_join(resolved, resolved_size, root, path);
	if (rc != 0)
		return rc;
	return 0;
}

static int patch_parent_ensure(const char *path)
{
	char parent[PATH_MAX];
	char *slash;

	if (strlen(path) >= sizeof(parent))
		MORPH_RETURN(-ENAMETOOLONG);
	strncpy(parent, path, sizeof(parent) - 1);
	parent[sizeof(parent) - 1] = '\0';
	slash = strrchr(parent, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	return file_ensure_dir(parent);
}

static void patch_line_trim(const char **text, size_t *len, int leading)
{
	while (*len > 0 && ((*text)[*len - 1] == ' ' ||
			    (*text)[*len - 1] == '\t'))
		(*len)--;
	if (!leading)
		return;
	while (*len > 0 && (**text == ' ' || **text == '\t')) {
		(*text)++;
		(*len)--;
	}
}

static int patch_normalize_line(const char *text, morph_buf_t *normalized)
{
	const utf8_int8_t *cursor = (const utf8_int8_t *)text;
	int rc = morph_buf_init(normalized, strlen(text) + 1);

	if (rc != 0)
		return rc;
	while (*cursor) {
		utf8_int32_t codepoint;
		const utf8_int8_t *next = utf8codepoint(cursor, &codepoint);
		char replacement = '\0';

		switch (codepoint) {
		case 0x2010: case 0x2011: case 0x2012: case 0x2013:
		case 0x2014: case 0x2015: case 0x2212:
			replacement = '-';
			break;
		case 0x2018: case 0x2019: case 0x201a: case 0x201b:
			replacement = '\'';
			break;
		case 0x201c: case 0x201d: case 0x201e: case 0x201f:
			replacement = '"';
			break;
		case 0x00a0: case 0x2002: case 0x2003: case 0x2004:
		case 0x2005: case 0x2006: case 0x2007: case 0x2008:
		case 0x2009: case 0x200a: case 0x202f: case 0x205f:
		case 0x3000:
			replacement = ' ';
			break;
		default:
			break;
		}
		if (replacement)
			rc = morph_buf_putc(normalized, replacement);
		else
			rc = morph_buf_append(normalized, cursor,
				(size_t)(next - cursor));
		if (rc != 0) {
			morph_buf_cleanup(normalized);
			return rc;
		}
		cursor = next;
	}
	return 0;
}

static int patch_lines_equal(const char *left, const char *right, int mode)
{
	const char *left_text = left;
	const char *right_text = right;
	size_t left_len = strlen(left);
	size_t right_len = strlen(right);

	if (mode < 3) {
		if (mode > 0) {
			patch_line_trim(&left_text, &left_len, mode > 1);
			patch_line_trim(&right_text, &right_len, mode > 1);
		}
		return left_len == right_len &&
			memcmp(left_text, right_text, left_len) == 0;
	}
	{
		morph_buf_t left_normalized;
		morph_buf_t right_normalized;
		int equal;
		int normalize_rc;

		memset(&left_normalized, 0, sizeof(left_normalized));
		memset(&right_normalized, 0, sizeof(right_normalized));
		patch_trim_view(left, &left_text, &left_len);
		patch_trim_view(right, &right_text, &right_len);
		char *left_copy = strndup(left_text, left_len);
		char *right_copy = strndup(right_text, right_len);
		if (!left_copy || !right_copy) {
			free(left_copy);
			free(right_copy);
			return 0;
		}
		normalize_rc = patch_normalize_line(left_copy, &left_normalized);
		if (normalize_rc == 0)
			normalize_rc = patch_normalize_line(right_copy,
				&right_normalized);
		if (normalize_rc != 0) {
			morph_buf_cleanup(&left_normalized);
			morph_buf_cleanup(&right_normalized);
			free(left_copy);
			free(right_copy);
			return 0;
		}
		equal = strcmp(morph_buf_cstr(&left_normalized),
			morph_buf_cstr(&right_normalized)) == 0;
		morph_buf_cleanup(&left_normalized);
		morph_buf_cleanup(&right_normalized);
		free(left_copy);
		free(right_copy);
		return equal;
	}
}

static int patch_seek_sequence(morph_array_t *lines, morph_array_t *pattern,
			       size_t start, int end_of_file, size_t *found)
{
	char **source = lines->elts;
	char **wanted = pattern->elts;
	size_t search_start;

	if (pattern->nelts == 0) {
		*found = start;
		return 1;
	}
	if (pattern->nelts > lines->nelts)
		return 0;
	search_start = end_of_file ? lines->nelts - pattern->nelts : start;
	if (search_start > lines->nelts - pattern->nelts)
		return 0;
	for (int mode = 0; mode < 4; mode++) {
		for (size_t i = search_start;
		     i <= lines->nelts - pattern->nelts; i++) {
			int matches = 1;

			for (size_t j = 0; j < pattern->nelts; j++) {
				if (!patch_lines_equal(source[i + j], wanted[j], mode)) {
					matches = 0;
					break;
				}
			}
			if (matches) {
				*found = i;
				return 1;
			}
		}
	}
	return 0;
}

static int patch_content_lines(const char *content, morph_array_t *lines)
{
	const char *start = content;
	const char *cursor = content;
	int rc = patch_string_array_init(lines);

	if (rc != 0)
		return rc;
	while (1) {
		if (*cursor == '\n' || *cursor == '\0') {
			size_t len = (size_t)(cursor - start);

			if (*cursor == '\0' && len == 0)
				break;
			rc = patch_string_array_push(lines, start, len);
			if (rc != 0) {
				patch_string_array_cleanup(lines);
				return rc;
			}
			if (*cursor == '\0')
				break;
			start = cursor + 1;
		}
		cursor++;
	}
	return 0;
}

static int patch_compute_replacements(morph_array_t *source,
				      struct patch_hunk *hunk,
				      morph_array_t *replacements,
				      char *error, size_t error_size)
{
	size_t cursor = 0;
	struct patch_chunk *chunk;

	morph_array_foreach(chunk, &hunk->chunks, struct patch_chunk) {
		struct patch_replacement *replacement;
		size_t found;
		size_t chunk_cursor = cursor;
		morph_array_t *old_lines = &chunk->old_lines;
		size_t old_count = old_lines->nelts;
		size_t new_count = chunk->new_lines.nelts;
		int matched;

		if (chunk->context) {
			morph_array_t context;
			char *context_value = chunk->context;

			memset(&context, 0, sizeof(context));
			if (morph_array_init(&context, 1, sizeof(char *)) != 0)
				MORPH_RETURN(-ENOMEM);
			char **slot = morph_array_push(&context);
			if (!slot) {
				morph_array_cleanup(&context);
				MORPH_RETURN(-ENOMEM);
			}
			*slot = context_value;
			matched = patch_seek_sequence(source, &context, cursor, 0,
				&found);
			morph_array_cleanup(&context);
			if (matched)
				cursor = found + 1;
		}
		if (old_count == 0) {
			found = source->nelts;
		} else {
			matched = patch_seek_sequence(source, old_lines, cursor,
				chunk->end_of_file, &found);
			if (!matched && cursor != chunk_cursor)
				matched = patch_seek_sequence(source, old_lines,
					chunk_cursor, chunk->end_of_file, &found);
			if (!matched && old_count > 0 &&
			    ((char **)old_lines->elts)[old_count - 1][0] == '\0') {
				old_lines->nelts--;
				old_count--;
				if (new_count > 0 &&
				    ((char **)chunk->new_lines.elts)[new_count - 1][0] == '\0')
					new_count--;
				matched = patch_seek_sequence(source, old_lines, cursor,
					chunk->end_of_file, &found);
				if (!matched && cursor != chunk_cursor)
					matched = patch_seek_sequence(source, old_lines,
						chunk_cursor, chunk->end_of_file,
						&found);
				old_lines->nelts++;
			}
			if (!matched)
				return patch_error(error, error_size, -EINVAL,
					"failed to find expected lines in %s", hunk->path);
			cursor = found + old_count;
		}
		replacement = morph_array_push(replacements);
		if (!replacement)
			MORPH_RETURN(-ENOMEM);
		*replacement = (struct patch_replacement){
			.start = found,
			.old_count = old_count,
			.order = replacements->nelts - 1,
			.chunk = chunk,
			.new_count = new_count,
		};
	}
	return 0;
}

static int patch_replacement_compare(const void *left, const void *right)
{
	const struct patch_replacement *a = left;
	const struct patch_replacement *b = right;

	if (a->start < b->start)
		return -1;
	if (a->start > b->start)
		return 1;
	if (a->order < b->order)
		return -1;
	if (a->order > b->order)
		return 1;
	return 0;
}

static int patch_build_updated_content(const char *original,
				       struct patch_hunk *hunk,
				       char **updated, char *error,
				       size_t error_size)
{
	morph_array_t source;
	morph_array_t replacements;
	morph_buf_t output;
	size_t source_index = 0;
	int rc;

	memset(&source, 0, sizeof(source));
	memset(&replacements, 0, sizeof(replacements));
	rc = patch_content_lines(original, &source);
	if (rc != 0)
		return rc;
	rc = morph_array_init(&replacements, hunk->chunks.nelts,
		sizeof(struct patch_replacement));
	if (rc != 0) {
		patch_string_array_cleanup(&source);
		return rc;
	}
	rc = patch_compute_replacements(&source, hunk, &replacements, error,
		error_size);
	if (rc != 0) {
		morph_array_cleanup(&replacements);
		patch_string_array_cleanup(&source);
		return rc;
	}
	qsort(replacements.elts, replacements.nelts,
		sizeof(struct patch_replacement), patch_replacement_compare);
	rc = morph_buf_init(&output, strlen(original) + 128);
	if (rc != 0) {
		morph_array_cleanup(&replacements);
		patch_string_array_cleanup(&source);
		return rc;
	}
	for (size_t i = 0; i < replacements.nelts; i++) {
		struct patch_replacement *replacement =
			morph_array_get(&replacements, i);
		char **source_lines = source.elts;
		char **new_lines = replacement->chunk->new_lines.elts;

		while (source_index < replacement->start) {
			rc = morph_buf_printf(&output, "%s\n", source_lines[source_index]);
			if (rc != 0)
				break;
			source_index++;
		}
		if (rc != 0)
			break;
		for (size_t j = 0; j < replacement->new_count; j++) {
			rc = morph_buf_printf(&output, "%s\n", new_lines[j]);
			if (rc != 0)
				break;
		}
		if (rc != 0)
			break;
		source_index = replacement->start + replacement->old_count;
	}
	if (rc == 0) {
		char **source_lines = source.elts;

		while (source_index < source.nelts) {
			rc = morph_buf_printf(&output, "%s\n", source_lines[source_index]);
			if (rc != 0)
				break;
			source_index++;
		}
	}
	if (rc == 0) {
		*updated = morph_buf_detach(&output);
		if (!*updated)
			rc = -ENOMEM;
	}
	morph_buf_cleanup(&output);
	morph_array_cleanup(&replacements);
	patch_string_array_cleanup(&source);
	return rc;
}

static int patch_atomic_write(const char *path, const char *content, mode_t mode)
{
	char temporary[PATH_MAX];
	int fd;
	size_t len = strlen(content);
	size_t written = 0;

	if (snprintf(temporary, sizeof(temporary), "%s.morph-patch-XXXXXX",
		path) >= (int)sizeof(temporary))
		MORPH_RETURN(-ENAMETOOLONG);
	fd = mkstemp(temporary);
	if (fd < 0)
		MORPH_RETURN(-errno);
	if (fchmod(fd, mode) != 0) {
		int rc = -errno;
		(void)close(fd);
		(void)unlink(temporary);
		return rc;
	}
	while (written < len) {
		ssize_t count = write(fd, content + written, len - written);

		if (count < 0) {
			int rc = -errno;
			(void)close(fd);
			(void)unlink(temporary);
			return rc;
		}
		written += (size_t)count;
	}
	if (fsync(fd) != 0) {
		int rc = -errno;
		(void)close(fd);
		(void)unlink(temporary);
		return rc;
	}
	if (close(fd) != 0) {
		int rc = -errno;
		(void)unlink(temporary);
		return rc;
	}
	if (rename(temporary, path) != 0) {
		int rc = -errno;
		(void)unlink(temporary);
		return rc;
	}
	return 0;
}

static int patch_result_add(struct patch_result *result,
			    struct patch_hunk *hunk, const char *path)
{
	struct patch_change *change = morph_array_push(&result->changes);

	if (!change)
		MORPH_RETURN(-ENOMEM);
	memset(change, 0, sizeof(*change));
	if (strlen(path) >= sizeof(change->path)) {
		result->changes.nelts--;
		MORPH_RETURN(-ENAMETOOLONG);
	}
	strncpy(change->path, path, sizeof(change->path) - 1);
	change->action = hunk->action;
	change->added = hunk->added;
	change->removed = hunk->removed;
	return 0;
}

static int patch_apply_hunk(const char *root, struct patch_hunk *hunk,
			    struct patch_result *result, char *error,
			    size_t error_size)
{
	char source_path[PATH_MAX];
	char destination_path[PATH_MAX];
	struct stat source_stat;
	int source_exists;
	int rc;

	rc = patch_resolve_target(root, hunk->path, source_path,
		sizeof(source_path), error, error_size);
	if (rc != 0)
		return rc;
	source_exists = lstat(source_path, &source_stat) == 0;
	if (source_exists && !S_ISREG(source_stat.st_mode))
		return patch_error(error, error_size, -EINVAL,
			"target is not a regular file: %s", hunk->path);
	if (hunk->action == PATCH_ACTION_ADD) {
		mode_t mode = source_exists ? source_stat.st_mode & 0777 : 0644;

		rc = patch_parent_ensure(source_path);
		if (rc == 0)
			rc = patch_atomic_write(source_path, hunk->add_content, mode);
		if (rc == 0)
			rc = patch_result_add(result, hunk, hunk->path);
	} else if (hunk->action == PATCH_ACTION_DELETE) {
		if (!source_exists)
			return patch_error(error, error_size, -ENOENT,
				"failed to delete file %s", source_path);
		rc = unlink(source_path) == 0 ? 0 : -errno;
		if (rc == 0)
			rc = patch_result_add(result, hunk, hunk->path);
	} else {
		char *original;
		char *updated = NULL;

		if (!source_exists)
			return patch_error(error, error_size, -ENOENT,
				"failed to read file to update %s", source_path);
		original = file_read_all(source_path, NULL);
		if (!original)
			return patch_error(error, error_size, -errno,
				"failed to read file to update %s", source_path);
		rc = patch_build_updated_content(original, hunk, &updated, error,
			error_size);
		free(original);
		if (rc != 0)
			return rc;
		if (hunk->move_path) {
			struct stat destination_stat;
			mode_t mode = source_stat.st_mode & 0777;

			rc = patch_resolve_target(root, hunk->move_path,
				destination_path, sizeof(destination_path), error,
				error_size);
			if (rc == 0 && lstat(destination_path, &destination_stat) == 0) {
				if (!S_ISREG(destination_stat.st_mode))
					rc = -EINVAL;
				else
					mode = destination_stat.st_mode & 0777;
			}
			if (rc == 0)
				rc = patch_parent_ensure(destination_path);
			if (rc == 0)
				rc = patch_atomic_write(destination_path, updated, mode);
			if (rc == 0 && strcmp(source_path, destination_path) != 0)
				rc = unlink(source_path) == 0 ? 0 : -errno;
			if (rc == 0)
				rc = patch_result_add(result, hunk, hunk->move_path);
		} else {
			rc = patch_atomic_write(source_path, updated,
				source_stat.st_mode & 0777);
			if (rc == 0)
				rc = patch_result_add(result, hunk, hunk->path);
		}
		free(updated);
	}
	if (rc != 0)
		return patch_error(error, error_size, rc,
			"failed to apply %s", hunk->path);
	return 0;
}

int patch_apply(const char *workdir, const char *input,
		struct patch_result *result, char *error, size_t error_size)
{
	morph_array_t hunks;
	char *root;
	char *copy;
	int rc;

	if (!workdir || !input || !result)
		MORPH_RETURN(-EINVAL);
	memset(result, 0, sizeof(*result));
	memset(&hunks, 0, sizeof(hunks));
	if (error && error_size > 0)
		error[0] = '\0';
	if (strlen(input) > PATCH_MAX_BYTES)
		return patch_error(error, error_size, -EFBIG,
			"patch exceeds the 64 KiB limit");
	if (utf8valid(input) != NULL)
		return patch_error(error, error_size, -EINVAL,
			"patch is not valid UTF-8");
	root = file_resolve_path(workdir);
	if (!root)
		MORPH_RETURN(-ENOMEM);
	copy = strdup(input);
	if (!copy) {
		free(root);
		MORPH_RETURN(-ENOMEM);
	}
	rc = morph_array_init(&hunks, 4, sizeof(struct patch_hunk));
	if (rc == 0)
		rc = patch_parse(copy, &hunks, error, error_size);
	if (rc == 0)
		rc = morph_array_init(&result->changes, hunks.nelts,
			sizeof(struct patch_change));
	if (rc == 0) {
		struct patch_hunk *hunk;

		morph_array_foreach(hunk, &hunks, struct patch_hunk) {
			rc = patch_apply_hunk(root, hunk, result, error, error_size);
			if (rc != 0)
				break;
		}
	}
	patch_hunks_cleanup(&hunks);
	free(copy);
	free(root);
	return rc;
}

void patch_result_cleanup(struct patch_result *result)
{
	if (!result)
		return;
	morph_array_cleanup(&result->changes);
	memset(result, 0, sizeof(*result));
}

const char *patch_action_name(enum patch_action action)
{
	switch (action) {
	case PATCH_ACTION_ADD:
		return "add";
	case PATCH_ACTION_UPDATE:
		return "update";
	case PATCH_ACTION_DELETE:
		return "delete";
	default:
		return "unknown";
	}
}
