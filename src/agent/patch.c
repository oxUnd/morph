#include "patch.h"
#include "nb/cleanup.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/utf8.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATCH_MAX_BYTES (64 * 1024)
#define PATCH_DIFF_CONTEXT 1
#define PATCH_DIFF_CELLS_MAX (1u << 20)

struct patch_chunk {
	char *context;
	nb_vec old_lines;
	nb_vec new_lines;
	size_t line_number;
	int end_of_file;
};

struct patch_hunk {
	enum patch_action action;
	char *path;
	char *move_path;
	char *add_content;
	nb_vec chunks;
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

static int patch_string_array_init(nb_vec *array)
{
	memset(array, 0, sizeof(*array));
	return (nb_vec_init(array, sizeof(char *)), nb_vec_reserve(array, 4) ? 0 : NB_ERR_NOMEM);
}

static void patch_string_array_cleanup(nb_vec *array)
{
	char **value;

	if (!array)
		return;
	for (size_t _i_ = 0; _i_ < nb_vec_len(array) && (value = (char **)nb_vec_at(array, _i_));
	     _i_++)
		free(*value);
	nb_vec_cleanup(array);
}

static int patch_string_array_push(nb_vec *array, const char *text, size_t len)
{
	char **slot = nb_vec_push(array);

	if (!slot)
		return NB_ERR_NOMEM;
	*slot = strndup(text, len);
	if (!*slot) {
		array->len--;
		return NB_ERR_NOMEM;
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

static void patch_hunks_cleanup(nb_vec *hunks)
{
	struct patch_hunk *hunk;

	if (!hunks)
		return;
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(hunks) && (hunk = (struct patch_hunk *)nb_vec_at(hunks, _i_));
	     _i_++) {
		free(hunk->path);
		free(hunk->move_path);
		free(hunk->add_content);
		for (size_t chunk_index = 0; chunk_index < hunk->chunks.len; chunk_index++) {
			struct patch_chunk *chunk = nb_vec_at(&hunk->chunks, chunk_index);

			patch_chunk_cleanup(chunk);
		}
		nb_vec_cleanup(&hunk->chunks);
	}
	nb_vec_cleanup(hunks);
}

static int patch_split_lines(char *copy, nb_vec *lines)
{
	char *start = copy;
	char *cursor = copy;

	while (1) {
		if (*cursor == '\r' && cursor[1] == '\n') {
			char **slot;

			*cursor = '\0';
			slot = nb_vec_push(lines);
			if (!slot)
				return NB_ERR_NOMEM;
			*slot = start;
			cursor += 2;
			start = cursor;
			continue;
		}
		if (*cursor == '\n' || *cursor == '\0') {
			char **slot;
			int done = *cursor == '\0';

			*cursor = '\0';
			slot = nb_vec_push(lines);
			if (!slot)
				return NB_ERR_NOMEM;
			*slot = start;
			if (done)
				break;
			start = cursor + 1;
		}
		cursor++;
	}
	return 0;
}

static int patch_append_add_line(nb_buf *content, const char *line)
{
	int rc;

	if (line[0] != '+')
		return NB_ERR_INVAL;
	rc = (nb_buf_append_cstr(content, line + 1) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_append_char(content, '\n') ? 0 : NB_ERR_NOMEM);
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
		return patch_error(error, error_size, NB_ERR_INVAL, "invalid hunk line %zu in %s",
				   line_number, hunk->path);
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
				return NB_ERR_NOMEM;
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
		chunk = nb_vec_push(&hunk->chunks);
		if (!chunk)
			return NB_ERR_NOMEM;
		rc = patch_chunk_init(chunk);
		if (rc != 0) {
			hunk->chunks.len--;
			return rc;
		}
		chunk->line_number = *index + 1;
		patch_trim_view(lines[*index], &trimmed, &trimmed_len);
		if (trimmed_len == 2 && memcmp(trimmed, "@@", 2) == 0) {
			(*index)++;
		} else if (trimmed_len > 3 && memcmp(trimmed, "@@ ", 3) == 0) {
			chunk->context = strndup(trimmed + 3, trimmed_len - 3);
			if (!chunk->context)
				return NB_ERR_NOMEM;
			(*index)++;
		} else if (hunk->chunks.len > 1) {
			return patch_error(error, error_size, NB_ERR_INVAL,
					   "expected @@ hunk header at line %zu in %s", *index + 1,
					   hunk->path);
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
		if (chunk->old_lines.len == 0 && chunk->new_lines.len == 0)
			return patch_error(error, error_size, NB_ERR_INVAL,
					   "empty update hunk at line %zu in %s", *index + 1,
					   hunk->path);
	}
	if (hunk->chunks.len == 0)
		return patch_error(error, error_size, NB_ERR_INVAL,
				   "update file hunk for path '%s' is empty", hunk->path);
	return 0;
}

static int patch_parse(char *copy, nb_vec *hunks, char *error, size_t error_size)
{
	nb_vec lines;
	char **line;
	size_t first = 0;
	size_t end;
	size_t index;
	int rc;

	memset(&lines, 0, sizeof(lines));
	rc = (nb_vec_init(&lines, sizeof(char *)), nb_vec_reserve(&lines, 64) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = patch_split_lines(copy, &lines);
	if (rc != 0) {
		nb_vec_cleanup(&lines);
		return rc;
	}
	line = ((void *)lines.data);
	end = lines.len;
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
		rc = patch_error(error, error_size, NB_ERR_INVAL,
				 "patch must start with *** Begin Patch");
		nb_vec_cleanup(&lines);
		return rc;
	}
	if (patch_header_kind(line[end - 1], NULL, NULL) != PATCH_HEADER_END) {
		rc = patch_error(error, error_size, NB_ERR_INVAL,
				 "patch must end with *** End Patch");
		nb_vec_cleanup(&lines);
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
			rc = patch_error(error, error_size, NB_ERR_INVAL,
					 "invalid patch hunk on line %zu: %s", index + 1,
					 line[index]);
			break;
		}
		hunk = nb_vec_push(hunks);
		if (!hunk) {
			rc = NB_ERR_NOMEM;
			break;
		}
		memset(hunk, 0, sizeof(*hunk));
		hunk->action = header == PATCH_HEADER_ADD ? PATCH_ACTION_ADD :
			header == PATCH_HEADER_UPDATE ? PATCH_ACTION_UPDATE :
			PATCH_ACTION_DELETE;
		hunk->path = strndup(value, value_len);
		if (!hunk->path) {
			rc = NB_ERR_NOMEM;
			break;
		}
		rc = (nb_vec_init(&hunk->chunks, sizeof(struct patch_chunk)),
		      nb_vec_reserve(&hunk->chunks, 2) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			break;
		index++;
		if (hunk->action == PATCH_ACTION_ADD) {
			nb_buf content NB_CLEANUP(nb_buf_cleanup) = {0};

			rc = (nb_buf_init(&content),
			      nb_buf_reserve(&content, 128) ? 0 : NB_ERR_NOMEM);
			if (rc != 0)
				break;
			while (index < end - 1 &&
			       patch_header_kind(line[index], NULL, NULL) ==
			       PATCH_HEADER_NONE) {
				rc = patch_append_add_line(&content, line[index]);
				if (rc == NB_ERR_INVAL) {
					rc = patch_error(error, error_size, NB_ERR_INVAL,
							 "invalid add-file line %zu in %s: "
							 "content lines must start with '+'",
							 index + 1, hunk->path);
					break;
				}
				if (rc != 0)
					break;
				hunk->added++;
				index++;
			}
			if (rc == 0 && hunk->added == 0)
				rc = patch_error(error, error_size, NB_ERR_INVAL,
						 "add file has no content lines: %s", hunk->path);
			if (rc == 0)
				hunk->add_content =
					(nb_buf_ok(&content) ? strdup(nb_buf_cstr(&content))
							     : NULL);
			nb_buf_cleanup(&content);
		} else if (hunk->action == PATCH_ACTION_UPDATE) {
			rc = patch_parse_update(line, &index, end - 1, hunk,
				error, error_size);
		}
		if (rc != 0)
			break;
	}
	if (rc == 0 && hunks->len == 0)
		rc = patch_error(error, error_size, NB_ERR_INVAL, "No files were modified.");
	nb_vec_cleanup(&lines);
	return rc;
}

static int patch_path_valid(const char *path)
{
	const char *part;

	if (!path || !*path || nb_path_is_absolute(path))
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
		return NB_ERR_NAME_TOO_LONG;
	strncpy(current, root, sizeof(current) - 1);
	current[sizeof(current) - 1] = '\0';
	while (*part) {
		const char *slash = strchr(part, '/');
		size_t len = slash ? (size_t)(slash - part) : strlen(part);
		size_t used = strlen(current);

		if (used + len + 2 > sizeof(current))
			return NB_ERR_NAME_TOO_LONG;
		current[used++] = '/';
		memcpy(current + used, part, len);
		current[used + len] = '\0';
		if (lstat(current, &st) == 0 && S_ISLNK(st.st_mode))
			return patch_error(error, error_size, NB_ERR_LOOP,
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
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;

	int rc;

	if (!patch_path_valid(path))
		return patch_error(error, error_size, NB_ERR_INVAL, "invalid relative path: %s",
				   path ? path : "(null)");
	rc = patch_reject_symlink_components(root, path, error, error_size);
	if (rc != 0)
		return rc;
	rc = ((file_status_1 = nb_path_concat(&file_value_1, root, path)) != NB_OK ? file_status_1
	      : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), resolved,
						resolved_size)) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_1);
	if (rc != 0)
		return rc;
	return 0;
}

static int patch_parent_ensure(const char *path)
{
	char parent[PATH_MAX];
	char *slash;

	if (strlen(path) >= sizeof(parent))
		return NB_ERR_NAME_TOO_LONG;
	strncpy(parent, path, sizeof(parent) - 1);
	parent[sizeof(parent) - 1] = '\0';
	slash = strrchr(parent, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	return nb_file_mkdir(parent, true, NULL);
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

static int patch_normalize_line(const char *text, nb_buf *normalized)
{
	const char *cursor = (const char *)text;
	int rc = (nb_buf_init(normalized),
		  nb_buf_reserve(normalized, strlen(text) + 1) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	while (*cursor) {
		int32_t codepoint;
		const char *next = nb_utf8_next_cstr(cursor, &codepoint);
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
			rc = (nb_buf_append_char(normalized, replacement) ? 0 : NB_ERR_NOMEM);
		else
			rc = (nb_buf_append(normalized, cursor, (size_t)(next - cursor))
				      ? 0
				      : NB_ERR_NOMEM);
		if (rc != 0) {
			nb_buf_cleanup(normalized);
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
		nb_buf left_normalized NB_CLEANUP(nb_buf_cleanup) = {0};
		nb_buf right_normalized NB_CLEANUP(nb_buf_cleanup) = {0};
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
			nb_buf_cleanup(&left_normalized);
			nb_buf_cleanup(&right_normalized);
			free(left_copy);
			free(right_copy);
			return 0;
		}
		equal = strcmp(nb_buf_cstr(&left_normalized), nb_buf_cstr(&right_normalized)) == 0;
		nb_buf_cleanup(&left_normalized);
		nb_buf_cleanup(&right_normalized);
		free(left_copy);
		free(right_copy);
		return equal;
	}
}

static int patch_seek_sequence(nb_vec *lines, nb_vec *pattern, size_t start, int end_of_file,
			       size_t *found)
{
	char **source = ((void *)lines->data);
	char **wanted = ((void *)pattern->data);
	size_t search_start;

	if (pattern->len == 0) {
		*found = start;
		return 1;
	}
	if (pattern->len > lines->len)
		return 0;
	search_start = end_of_file ? lines->len - pattern->len : start;
	if (search_start > lines->len - pattern->len)
		return 0;
	for (int mode = 0; mode < 4; mode++) {
		for (size_t i = search_start; i <= lines->len - pattern->len; i++) {
			int matches = 1;

			for (size_t j = 0; j < pattern->len; j++) {
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

static int patch_content_lines(const char *content, nb_vec *lines)
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

static int patch_compute_replacements(nb_vec *source, struct patch_hunk *hunk, nb_vec *replacements,
				      char *error, size_t error_size)
{
	size_t cursor = 0;
	struct patch_chunk *chunk;

	for (size_t _i_ = 0; _i_ < nb_vec_len(&hunk->chunks) &&
			     (chunk = (struct patch_chunk *)nb_vec_at(&hunk->chunks, _i_));
	     _i_++) {
		struct patch_replacement *replacement;
		size_t found;
		size_t chunk_cursor = cursor;
		nb_vec *old_lines = &chunk->old_lines;
		size_t old_count = old_lines->len;
		size_t new_count = chunk->new_lines.len;
		int matched;

		if (chunk->context) {
			nb_vec context;
			char *context_value = chunk->context;

			memset(&context, 0, sizeof(context));
			if ((nb_vec_init(&context, sizeof(char *)),
			     nb_vec_reserve(&context, 1) ? 0 : NB_ERR_NOMEM) != 0)
				return NB_ERR_NOMEM;
			char **slot = nb_vec_push(&context);
			if (!slot) {
				nb_vec_cleanup(&context);
				return NB_ERR_NOMEM;
			}
			*slot = context_value;
			matched = patch_seek_sequence(source, &context, cursor, 0,
				&found);
			nb_vec_cleanup(&context);
			if (matched)
				cursor = found + 1;
		}
		if (old_count == 0) {
			found = source->len;
		} else {
			matched = patch_seek_sequence(source, old_lines, cursor,
				chunk->end_of_file, &found);
			if (!matched && cursor != chunk_cursor)
				matched = patch_seek_sequence(source, old_lines,
					chunk_cursor, chunk->end_of_file, &found);
			if (!matched && old_count > 0 &&
			    ((char **)((void *)old_lines->data))[old_count - 1][0] == '\0') {
				old_lines->len--;
				old_count--;
				if (new_count > 0 &&
				    ((char **)((void *)chunk->new_lines.data))[new_count - 1][0] ==
					    '\0')
					new_count--;
				matched = patch_seek_sequence(source, old_lines, cursor,
					chunk->end_of_file, &found);
				if (!matched && cursor != chunk_cursor)
					matched = patch_seek_sequence(source, old_lines,
						chunk_cursor, chunk->end_of_file,
						&found);
				old_lines->len++;
			}
			if (!matched)
				return patch_error(error, error_size, NB_ERR_INVAL,
						   "failed to find expected lines from patch "
						   "line %zu in %s",
						   chunk->line_number, hunk->path);
			cursor = found + old_count;
		}
		replacement = nb_vec_push(replacements);
		if (!replacement)
			return NB_ERR_NOMEM;
		*replacement = (struct patch_replacement){
			.start = found,
			.old_count = old_count,
			.order = replacements->len - 1,
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
	nb_vec source;
	nb_vec replacements;
	nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
	size_t source_index = 0;
	int rc;

	memset(&source, 0, sizeof(source));
	memset(&replacements, 0, sizeof(replacements));
	rc = patch_content_lines(original, &source);
	if (rc != 0)
		return rc;
	rc = (nb_vec_init(&replacements, sizeof(struct patch_replacement)),
	      nb_vec_reserve(&replacements, hunk->chunks.len) ? 0 : NB_ERR_NOMEM);
	if (rc != 0) {
		patch_string_array_cleanup(&source);
		return rc;
	}
	rc = patch_compute_replacements(&source, hunk, &replacements, error,
		error_size);
	if (rc != 0) {
		nb_vec_cleanup(&replacements);
		patch_string_array_cleanup(&source);
		return rc;
	}
	qsort(((void *)replacements.data), replacements.len, sizeof(struct patch_replacement),
	      patch_replacement_compare);
	rc = (nb_buf_init(&output),
	      nb_buf_reserve(&output, strlen(original) + 128) ? 0 : NB_ERR_NOMEM);
	if (rc != 0) {
		nb_vec_cleanup(&replacements);
		patch_string_array_cleanup(&source);
		return rc;
	}
	for (size_t i = 0; i < replacements.len; i++) {
		struct patch_replacement *replacement = nb_vec_at(&replacements, i);
		char **source_lines = ((void *)source.data);
		char **new_lines = ((void *)replacement->chunk->new_lines.data);

		while (source_index < replacement->start) {
			rc = (nb_buf_printf(&output, "%s\n", source_lines[source_index])
				      ? 0
				      : NB_ERR_NOMEM);
			if (rc != 0)
				break;
			source_index++;
		}
		if (rc != 0)
			break;
		for (size_t j = 0; j < replacement->new_count; j++) {
			rc = (nb_buf_printf(&output, "%s\n", new_lines[j]) ? 0 : NB_ERR_NOMEM);
			if (rc != 0)
				break;
		}
		if (rc != 0)
			break;
		source_index = replacement->start + replacement->old_count;
	}
	if (rc == 0) {
		char **source_lines = ((void *)source.data);

		while (source_index < source.len) {
			rc = (nb_buf_printf(&output, "%s\n", source_lines[source_index])
				      ? 0
				      : NB_ERR_NOMEM);
			if (rc != 0)
				break;
			source_index++;
		}
	}
	if (rc == 0) {
		*updated = (nb_buf_ok(&output) ? strdup(nb_buf_cstr(&output)) : NULL);
		if (!*updated)
			rc = NB_ERR_NOMEM;
	}
	nb_buf_cleanup(&output);
	nb_vec_cleanup(&replacements);
	patch_string_array_cleanup(&source);
	return rc;
}

enum patch_diff_kind {
	PATCH_DIFF_COMMON,
	PATCH_DIFF_REMOVE,
	PATCH_DIFF_ADD,
};

struct patch_diff_op {
	enum patch_diff_kind kind;
	const char *text;
};

static int patch_diff_push(nb_vec *ops, enum patch_diff_kind kind, const char *text)
{
	struct patch_diff_op *op = nb_vec_push(ops);

	if (!op)
		return NB_ERR_NOMEM;
	op->kind = kind;
	op->text = text;
	return 0;
}

static int patch_diff_emit(nb_buf *out, char prefix, const char *line)
{
	int rc = (nb_buf_append_char(out, prefix) ? 0 : NB_ERR_NOMEM);

	if (rc == 0)
		rc = (nb_buf_append_cstr(out, line) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_append_char(out, '\n') ? 0 : NB_ERR_NOMEM);
	return rc;
}

/*
 * Compute a minimal line diff between two blocks with an LCS table so that
 * unchanged lines inside a chunk stay as context lines. Oversized blocks
 * fall back to a plain remove-then-add listing to bound memory.
 */
static int patch_diff_block(char **old_lines, size_t old_count, char **new_lines, size_t new_count,
			    nb_vec *ops)
{
	size_t base = ops->len;
	size_t rows = old_count + 1;
	size_t cols = new_count + 1;
	uint32_t *table = NULL;
	size_t i;
	size_t j;
	int rc = 0;

	if (rows * cols > PATCH_DIFF_CELLS_MAX) {
		for (i = 0; i < old_count; i++) {
			rc = patch_diff_push(ops, PATCH_DIFF_REMOVE, old_lines[i]);
			if (rc != 0)
				return rc;
		}
		for (j = 0; j < new_count; j++) {
			rc = patch_diff_push(ops, PATCH_DIFF_ADD, new_lines[j]);
			if (rc != 0)
				return rc;
		}
		return 0;
	}
	table = calloc(rows * cols, sizeof(*table));
	if (!table)
		return NB_ERR_NOMEM;
	for (i = 1; i < rows; i++) {
		for (j = 1; j < cols; j++) {
			if (strcmp(old_lines[i - 1], new_lines[j - 1]) == 0)
				table[i * cols + j] =
					table[(i - 1) * cols + (j - 1)] + 1;
			else
				table[i * cols + j] =
					table[(i - 1) * cols + j] >=
					table[i * cols + (j - 1)] ?
					table[(i - 1) * cols + j] :
					table[i * cols + (j - 1)];
		}
	}
	i = old_count;
	j = new_count;
	while (i > 0 && j > 0) {
		if (strcmp(old_lines[i - 1], new_lines[j - 1]) == 0) {
			rc = patch_diff_push(ops, PATCH_DIFF_COMMON,
				old_lines[i - 1]);
			i--;
			j--;
		} else if (table[(i - 1) * cols + j] >
			   table[i * cols + (j - 1)]) {
			rc = patch_diff_push(ops, PATCH_DIFF_REMOVE,
				old_lines[i - 1]);
			i--;
		} else {
			rc = patch_diff_push(ops, PATCH_DIFF_ADD,
				new_lines[j - 1]);
			j--;
		}
		if (rc != 0)
			break;
	}
	while (rc == 0 && i > 0)
		rc = patch_diff_push(ops, PATCH_DIFF_REMOVE, old_lines[--i]);
	while (rc == 0 && j > 0)
		rc = patch_diff_push(ops, PATCH_DIFF_ADD, new_lines[--j]);
	free(table);
	if (rc != 0)
		return rc;
	if (ops->len > base) {
		struct patch_diff_op *items = ((void *)ops->data);

		for (i = base, j = ops->len - 1; i < j; i++, j--) {
			struct patch_diff_op swap = items[i];

			items[i] = items[j];
			items[j] = swap;
		}
	}
	return 0;
}

static int patch_build_file_ops(nb_vec *source, nb_vec *replacements, nb_vec *ops)
{
	char **source_lines = ((void *)source->data);
	struct patch_replacement *items = ((void *)replacements->data);
	size_t old_index = 0;
	int rc;

	for (size_t k = 0; k < replacements->len; k++) {
		struct patch_replacement *replacement = &items[k];
		char **new_lines = ((void *)replacement->chunk->new_lines.data);

		while (old_index < replacement->start) {
			rc = patch_diff_push(ops, PATCH_DIFF_COMMON,
				source_lines[old_index]);
			if (rc != 0)
				return rc;
			old_index++;
		}
		rc = patch_diff_block(source_lines + replacement->start,
			replacement->old_count, new_lines, replacement->new_count,
			ops);
		if (rc != 0)
			return rc;
		old_index = replacement->start + replacement->old_count;
	}
	while (old_index < source->len) {
		rc = patch_diff_push(ops, PATCH_DIFF_COMMON,
			source_lines[old_index]);
		if (rc != 0)
			return rc;
		old_index++;
	}
	return 0;
}

static int patch_render_diff_hunks(nb_buf *out, nb_vec *ops)
{
	struct patch_diff_op *items = ((void *)ops->data);
	size_t count = ops->len;
	size_t index = 0;
	int rc = 0;

	while (index < count && rc == 0) {
		size_t hunk_start;
		size_t hunk_end;
		size_t begin;
		size_t end;
		size_t old_start = 1;
		size_t new_start = 1;
		size_t old_len = 0;
		size_t new_len = 0;
		size_t back;
		size_t forward;

		while (index < count && items[index].kind == PATCH_DIFF_COMMON)
			index++;
		if (index >= count)
			break;
		hunk_start = index;
		hunk_end = index;
		while (hunk_end < count) {
			size_t run_start = hunk_end;
			size_t run_end;

			while (run_start < count &&
			       items[run_start].kind == PATCH_DIFF_COMMON)
				run_start++;
			if (run_start >= count)
				break;
			run_end = run_start;
			while (run_end < count &&
			       items[run_end].kind != PATCH_DIFF_COMMON)
				run_end++;
			if (run_start - hunk_end > 2 * PATCH_DIFF_CONTEXT)
				break;
			hunk_end = run_end;
		}
		begin = hunk_start;
		end = hunk_end;
		back = 0;
		while (begin > 0 &&
		       items[begin - 1].kind == PATCH_DIFF_COMMON &&
		       back < PATCH_DIFF_CONTEXT) {
			begin--;
			back++;
		}
		forward = 0;
		while (end < count &&
		       items[end].kind == PATCH_DIFF_COMMON &&
		       forward < PATCH_DIFF_CONTEXT) {
			end++;
			forward++;
		}
		for (size_t i = 0; i < begin; i++) {
			if (items[i].kind != PATCH_DIFF_ADD)
				old_start++;
			if (items[i].kind != PATCH_DIFF_REMOVE)
				new_start++;
		}
		for (size_t i = begin; i < end; i++) {
			if (items[i].kind != PATCH_DIFF_ADD)
				old_len++;
			if (items[i].kind != PATCH_DIFF_REMOVE)
				new_len++;
		}
		rc = (nb_buf_printf(out, "@@ -%zu,%zu +%zu,%zu @@\n", old_start, old_len, new_start,
				    new_len)
			      ? 0
			      : NB_ERR_NOMEM);
		for (size_t i = begin; i < end && rc == 0; i++) {
			char prefix = items[i].kind == PATCH_DIFF_COMMON ? ' ' :
				items[i].kind == PATCH_DIFF_REMOVE ? '-' : '+';

			rc = patch_diff_emit(out, prefix, items[i].text);
		}
		index = end;
	}
	return rc;
}

static int patch_render_update_preview(nb_buf *out, nb_vec *source, struct patch_hunk *hunk,
				       char *error, size_t error_size)
{
	nb_vec replacements;
	nb_vec ops;
	int rc;

	memset(&replacements, 0, sizeof(replacements));
	memset(&ops, 0, sizeof(ops));
	rc = (nb_vec_init(&replacements, sizeof(struct patch_replacement)),
	      nb_vec_reserve(&replacements, hunk->chunks.len) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = patch_compute_replacements(source, hunk, &replacements, error,
		error_size);
	if (rc == 0)
		qsort(((void *)replacements.data), replacements.len,
		      sizeof(struct patch_replacement), patch_replacement_compare);
	if (rc == 0)
		rc = (nb_vec_init(&ops, sizeof(struct patch_diff_op)),
		      nb_vec_reserve(&ops, source->len + 8) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = patch_build_file_ops(source, &replacements, &ops);
	if (rc == 0)
		rc = patch_render_diff_hunks(out, &ops);
	nb_vec_cleanup(&ops);
	nb_vec_cleanup(&replacements);
	return rc;
}

static int patch_render_add_preview(nb_buf *out, struct patch_hunk *hunk)
{
	nb_vec lines;
	char **entries;
	int rc;

	rc = patch_content_lines(hunk->add_content, &lines);
	if (rc != 0)
		return rc;
	rc = (nb_buf_printf(out, "*** Add File: %s\n@@ -0,0 +1,%zu @@\n", hunk->path, lines.len)
		      ? 0
		      : NB_ERR_NOMEM);
	entries = ((void *)lines.data);
	for (size_t i = 0; rc == 0 && i < lines.len; i++)
		rc = patch_diff_emit(out, '+', entries[i]);
	patch_string_array_cleanup(&lines);
	return rc;
}

static int patch_render_delete_preview(nb_buf *out, const char *path, const char *resolved,
				       char *error, size_t error_size)
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_2 NB_CLEANUP(nb_charp_cleanup) = NULL;

	char *content =
		(nb_file_read_all(resolved, &file_value_2, 0, NULL) == NB_OK &&
				 (file_copy_2 = (char *)malloc(nb_buf_len(&file_value_2) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_2, nb_buf_cstr(&file_value_2),
				   nb_buf_len(&file_value_2) + 1),
			    nb_charp_steal(&file_copy_2))
			 : NULL);
	nb_vec lines;
	char **entries;
	int rc;

	if (!content)
		return patch_error(error, error_size, nb_status_from_errno(errno, NULL),
				   "failed to read file to delete %s", resolved);
	rc = patch_content_lines(content, &lines);
	free(content);
	if (rc != 0)
		return rc;
	rc = (nb_buf_printf(out, "*** Delete File: %s\n@@ -1,%zu +0,0 @@\n", path, lines.len)
		      ? 0
		      : NB_ERR_NOMEM);
	entries = ((void *)lines.data);
	for (size_t i = 0; rc == 0 && i < lines.len; i++)
		rc = patch_diff_emit(out, '-', entries[i]);
	patch_string_array_cleanup(&lines);
	return rc;
}

static int patch_render_hunk_preview(nb_buf *out, const char *root, struct patch_hunk *hunk,
				     char *error, size_t error_size)
{
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_3 NB_CLEANUP(nb_charp_cleanup) = NULL;

	char source_path[PATH_MAX];
	char *original;
	nb_vec source;
	int rc;

	rc = patch_resolve_target(root, hunk->path, source_path,
		sizeof(source_path), error, error_size);
	if (rc != 0)
		return rc;
	if (hunk->action == PATCH_ACTION_ADD)
		return patch_render_add_preview(out, hunk);
	if (hunk->action == PATCH_ACTION_DELETE)
		return patch_render_delete_preview(out, hunk->path, source_path,
			error, error_size);
	original = (nb_file_read_all(source_path, &file_value_3, 0, NULL) == NB_OK &&
				    (file_copy_3 = (char *)malloc(nb_buf_len(&file_value_3) + 1)) !=
					    NULL
			    ? (memcpy(file_copy_3, nb_buf_cstr(&file_value_3),
				      nb_buf_len(&file_value_3) + 1),
			       nb_charp_steal(&file_copy_3))
			    : NULL);
	if (!original)
		return patch_error(error, error_size, nb_status_from_errno(errno, NULL),
				   "failed to read file to update %s", source_path);
	rc = patch_content_lines(original, &source);
	free(original);
	if (rc != 0)
		return rc;
	rc = (nb_buf_printf(out, "*** Update File: %s\n", hunk->path) ? 0 : NB_ERR_NOMEM);
	if (rc == 0 && hunk->move_path)
		rc = (nb_buf_printf(out, "*** Move to: %s\n", hunk->move_path) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = patch_render_update_preview(out, &source, hunk, error,
			error_size);
	patch_string_array_cleanup(&source);
	return rc;
}

int patch_preview(const char *workdir, const char *input, nb_buf *out, char *error,
		  size_t error_size)
{
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};

	nb_vec hunks;
	struct patch_hunk *hunk;
	char *root;
	char *copy;
	int rc;

	if (!workdir || !input || !out)
		return NB_ERR_INVAL;
	memset(&hunks, 0, sizeof(hunks));
	if (error && error_size > 0)
		error[0] = '\0';
	if (strlen(input) > PATCH_MAX_BYTES)
		return patch_error(error, error_size, NB_ERR_TOO_BIG,
				   "patch exceeds the 64 KiB limit");
	if (nb_utf8_validate(nb_str_from_cstr(input), NULL) != NB_UTF8_OK)
		return patch_error(error, error_size, NB_ERR_INVAL, "patch is not valid UTF-8");
	root = (nb_path_expand(&file_value_4, workdir, NULL) == NB_OK &&
				nb_path_canonical_missing(&file_value_4, nb_buf_cstr(&file_value_4),
							  NULL) == NB_OK
			? strdup(nb_buf_cstr(&file_value_4))
			: NULL);
	if (!root)
		return NB_ERR_NOMEM;
	copy = strdup(input);
	if (!copy) {
		free(root);
		return NB_ERR_NOMEM;
	}
	rc = (nb_vec_init(&hunks, sizeof(struct patch_hunk)),
	      nb_vec_reserve(&hunks, 4) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = patch_parse(copy, &hunks, error, error_size);
	if (rc == 0) {
		for (size_t _i_ = 0; _i_ < nb_vec_len(&hunks) &&
				     (hunk = (struct patch_hunk *)nb_vec_at(&hunks, _i_));
		     _i_++) {
			rc = patch_render_hunk_preview(out, root, hunk, error,
				error_size);
			if (rc != 0)
				break;
		}
	}
	patch_hunks_cleanup(&hunks);
	free(copy);
	free(root);
	return rc;
}

static int patch_atomic_write(const char *path, const char *content, mode_t mode)
{
	int rc = nb_file_write_all_atomic(
		path, content, strlen(content),
		&(nb_file_write_options){.new_file_mode = 0600, .sync_dir = true}, NULL);
	if (rc == 0 && chmod(path, mode) < 0) {
		rc = nb_status_from_errno(errno, NULL);
	}
	return rc;
}

static int patch_result_add(struct patch_result *result,
			    struct patch_hunk *hunk, const char *path)
{
	struct patch_change *change = nb_vec_push(&result->changes);

	if (!change)
		return NB_ERR_NOMEM;
	memset(change, 0, sizeof(*change));
	if (strlen(path) >= sizeof(change->path)) {
		result->changes.len--;
		return NB_ERR_NAME_TOO_LONG;
	}
	strncpy(change->path, path, sizeof(change->path) - 1);
	change->action = hunk->action;
	change->added = hunk->added;
	change->removed = hunk->removed;
	return 0;
}

static int patch_validate_write(const struct patch_write_policy *policy,
				const char *path, size_t bytes, const char *removed_path,
				char *error, size_t error_size)
{
	if (!policy)
		return 0;
	if (policy->max_file_bytes && bytes > policy->max_file_bytes)
		return patch_error(error, error_size, NB_ERR_TOO_BIG,
				   "patched file exceeds the %zu-byte checkpoint limit: %s",
				   policy->max_file_bytes, path);
	int rc = policy->validate ?
		policy->validate(path, bytes, removed_path, policy->user_data) : 0;
	if (rc != 0)
		return patch_error(error, error_size, rc,
				   "checkpoint postimage budget rejected %s: %s", path,
				   nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	return 0;
}

static int patch_apply_hunk(const char *root, struct patch_hunk *hunk,
			    struct patch_result *result, char *error,
			    size_t error_size, const struct patch_write_policy *policy)
{
	nb_buf file_value_5 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_5 NB_CLEANUP(nb_charp_cleanup) = NULL;

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
		return patch_error(error, error_size, NB_ERR_INVAL,
				   "target is not a regular file: %s", hunk->path);
	if (hunk->action == PATCH_ACTION_ADD) {
		mode_t mode = source_exists ? source_stat.st_mode & 0777 : 0644;
		rc = patch_validate_write(policy, source_path, strlen(hunk->add_content), NULL,
			error, error_size);
		if (rc != 0)
			return rc;

		rc = patch_parent_ensure(source_path);
		if (rc == 0)
			rc = patch_atomic_write(source_path, hunk->add_content, mode);
		if (rc == 0)
			rc = patch_result_add(result, hunk, hunk->path);
	} else if (hunk->action == PATCH_ACTION_DELETE) {
		if (!source_exists)
			return patch_error(error, error_size, NB_ERR_NOT_FOUND,
					   "failed to delete file %s", source_path);
		rc = patch_validate_write(policy, source_path, 0, NULL, error, error_size);
		if (rc != 0)
			return rc;
		rc = unlink(source_path) == 0 ? 0 : nb_status_from_errno(errno, NULL);
		if (rc == 0)
			rc = patch_result_add(result, hunk, hunk->path);
	} else {
		char *original NB_CLEANUP(nb_charp_cleanup) = NULL;
		char *updated NB_CLEANUP(nb_charp_cleanup) = NULL;

		if (!source_exists)
			return patch_error(error, error_size, NB_ERR_NOT_FOUND,
					   "failed to read file to update %s", source_path);
		original = (nb_file_read_all(source_path, &file_value_5, 0, NULL) == NB_OK &&
					    (file_copy_5 = (char *)malloc(
						     nb_buf_len(&file_value_5) + 1)) != NULL
				    ? (memcpy(file_copy_5, nb_buf_cstr(&file_value_5),
					      nb_buf_len(&file_value_5) + 1),
				       nb_charp_steal(&file_copy_5))
				    : NULL);
		if (!original)
			return patch_error(error, error_size, nb_status_from_errno(errno, NULL),
					   "failed to read file to update %s", source_path);
		rc = patch_build_updated_content(original, hunk, &updated, error,
			error_size);
		nb_charp_cleanup(&original);
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
					rc = NB_ERR_INVAL;
				else
					mode = destination_stat.st_mode & 0777;
			}
			if (rc == 0)
				rc = patch_validate_write(policy, destination_path, strlen(updated),
					strcmp(source_path, destination_path) ? source_path : NULL,
					error, error_size);
			if (rc != 0)
				return rc;
			if (rc == 0)
				rc = patch_parent_ensure(destination_path);
			if (rc == 0)
				rc = patch_atomic_write(destination_path, updated, mode);
			if (rc == 0 && strcmp(source_path, destination_path) != 0)
				rc = unlink(source_path) == 0 ? 0
							      : nb_status_from_errno(errno, NULL);
			if (rc == 0)
				rc = patch_result_add(result, hunk, hunk->move_path);
		} else {
			rc = patch_validate_write(policy, source_path, strlen(updated), NULL,
				error, error_size);
			if (rc != 0)
				return rc;
			rc = patch_atomic_write(source_path, updated, source_stat.st_mode & 0777);
			if (rc == 0)
				rc = patch_result_add(result, hunk, hunk->path);
		}
	}
	if (rc != 0)
		return patch_error(error, error_size, rc,
			"failed to apply %s", hunk->path);
	return 0;
}

struct patch_permission_target {
	char path[PATH_MAX];
	int deletion;
};

static int patch_authorize_hunks(const char *root, const nb_vec *hunks, char *error,
				 size_t error_size, patch_authorize_fn authorize, void *user_data)
{
	nb_vec targets;
	struct patch_hunk *hunk;
	struct patch_permission_target *target;
	int rc = (nb_vec_init(&targets, sizeof(*target)),
		  nb_vec_reserve(&targets, hunks->len) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(hunks) && (hunk = (struct patch_hunk *)nb_vec_at(hunks, _i_));
	     _i_++) {
		target = nb_vec_push(&targets);
		if (!target) {
			rc = NB_ERR_NOMEM;
			break;
		}
		rc = patch_resolve_target(root, hunk->path, target->path,
			sizeof(target->path), error, error_size);
		target->deletion = hunk->action == PATCH_ACTION_DELETE || hunk->move_path != NULL;
		if (rc != 0)
			break;
		if (!hunk->move_path)
			continue;
		target = nb_vec_push(&targets);
		if (!target) {
			rc = NB_ERR_NOMEM;
			break;
		}
		rc = patch_resolve_target(root, hunk->move_path, target->path,
			sizeof(target->path), error, error_size);
		target->deletion = 0;
		if (rc != 0)
			break;
	}
	if (rc == 0) {
		for (size_t _i_ = 0;
		     _i_ < nb_vec_len(&targets) &&
		     (target = (struct patch_permission_target *)nb_vec_at(&targets, _i_));
		     _i_++) {
			rc = authorize(target->path, target->deletion, user_data);
			if (rc != 0)
				break;
		}
	}
	nb_vec_cleanup(&targets);
	return rc;
}

int patch_apply_authorized_with_policy(const char *workdir, const char *input,
		struct patch_result *result, char *error, size_t error_size,
		patch_authorize_fn authorize, void *user_data,
		const struct patch_write_policy *policy)
{
	nb_buf file_value_6 NB_CLEANUP(nb_buf_destroy) = {0};

	struct patch_result candidate NB_CLEANUP(patch_result_cleanup) = {0};
	nb_vec hunks;
	char *root;
	char *copy;
	int rc;

	if (!workdir || !input || !result)
		return NB_ERR_INVAL;
	if (((void *)result->changes.data) || result->changes.len)
		return NB_ERR_STATE;
	memset(&hunks, 0, sizeof(hunks));
	if (error && error_size > 0)
		error[0] = '\0';
	if (strlen(input) > PATCH_MAX_BYTES)
		return patch_error(error, error_size, NB_ERR_TOO_BIG,
				   "patch exceeds the 64 KiB limit");
	if (nb_utf8_validate(nb_str_from_cstr(input), NULL) != NB_UTF8_OK)
		return patch_error(error, error_size, NB_ERR_INVAL, "patch is not valid UTF-8");
	root = (nb_path_expand(&file_value_6, workdir, NULL) == NB_OK &&
				nb_path_canonical_missing(&file_value_6, nb_buf_cstr(&file_value_6),
							  NULL) == NB_OK
			? strdup(nb_buf_cstr(&file_value_6))
			: NULL);
	if (!root)
		return NB_ERR_NOMEM;
	copy = strdup(input);
	if (!copy) {
		free(root);
		return NB_ERR_NOMEM;
	}
	rc = (nb_vec_init(&hunks, sizeof(struct patch_hunk)),
	      nb_vec_reserve(&hunks, 4) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = patch_parse(copy, &hunks, error, error_size);
	if (rc == 0 && authorize)
		rc = patch_authorize_hunks(root, &hunks, error, error_size, authorize, user_data);
	if (rc == 0)
		rc = (nb_vec_init(&candidate.changes, sizeof(struct patch_change)),
		      nb_vec_reserve(&candidate.changes, hunks.len) ? 0 : NB_ERR_NOMEM);
	if (rc == 0) {
		struct patch_hunk *hunk;

		for (size_t _i_ = 0; _i_ < nb_vec_len(&hunks) &&
				     (hunk = (struct patch_hunk *)nb_vec_at(&hunks, _i_));
		     _i_++) {
			rc = patch_apply_hunk(root, hunk, &candidate, error, error_size, policy);
			if (rc != 0)
				break;
		}
	}
	patch_hunks_cleanup(&hunks);
	free(copy);
	free(root);
	if (rc == 0) {
		*result = candidate;
		memset(&candidate, 0, sizeof(candidate));
	}
	return rc;
}

int patch_apply_authorized_with_limit(const char *workdir, const char *input,
		struct patch_result *result, char *error, size_t error_size,
		patch_authorize_fn authorize, void *user_data, size_t max_file_bytes)
{
	struct patch_write_policy policy = {.max_file_bytes = max_file_bytes};

	return patch_apply_authorized_with_policy(workdir, input, result, error, error_size,
		authorize, user_data, &policy);
}

int patch_apply_authorized(const char *workdir, const char *input,
		struct patch_result *result, char *error, size_t error_size,
		patch_authorize_fn authorize, void *user_data)
{
	return patch_apply_authorized_with_limit(workdir, input, result, error, error_size,
						authorize, user_data, 0);
}

int patch_apply(const char *workdir, const char *input,
	struct patch_result *result, char *error, size_t error_size)
{
	return patch_apply_authorized(workdir, input, result, error, error_size, NULL, NULL);
}

void patch_result_cleanup(struct patch_result *result)
{
	if (!result)
		return;
	nb_vec_cleanup(&result->changes);
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
