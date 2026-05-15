#include "file_read.h"
#include "util/log.h"
#include "util/file.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define BINARY_CHECK_SIZE 4096
#define MAX_LINE_LENGTH 65536
#define MAX_CONTENT_SIZE (10 * 1024 * 1024)

struct line_buffer {
	char *data;
	size_t len;
	size_t cap;
};

static int buf_append(struct line_buffer *b, const char *s, size_t n)
{
	if (!n) return 0;
	if (b->len + n + 1 > b->cap) {
		size_t new_cap = b->cap ? b->cap * 2 : 65536;
		while (b->len + n + 1 > new_cap)
			new_cap *= 2;
		if (new_cap > MAX_CONTENT_SIZE)
			new_cap = MAX_CONTENT_SIZE;
		if (b->len + n + 1 > new_cap)
			return -1;
		char *tmp = realloc(b->data, new_cap);
		if (!tmp) return -1;
		b->data = tmp;
		b->cap = new_cap;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
	return 0;
}

static int is_binary(const char *data, size_t len)
{
	size_t check = len < BINARY_CHECK_SIZE ? len : BINARY_CHECK_SIZE;
	for (size_t i = 0; i < check; i++) {
		if ((unsigned char)data[i] == 0)
			return 1;
	}
	return 0;
}

static int file_read_exec(const char *args_json, char **result_json, void *user_data)
{
	(void)user_data;
	if (!result_json) return -EINVAL;

	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		*result_json = strdup("{\"error\":\"invalid JSON\"}");
		return -EINVAL;
	}

	cJSON *fp = cJSON_GetObjectItem(root, "file_path");
	const char *file_path = cJSON_IsString(fp) ? fp->valuestring : NULL;
	if (!file_path) {
		cJSON_Delete(root);
		*result_json = strdup(
			"{\"error\":\"missing 'file_path' parameter. "
			"Usage: file_read({\\\"file_path\\\": \\\"path/to/file\\\"})\"}");
		return -EINVAL;
	}

	long offset_val = 0;
	cJSON *off = cJSON_GetObjectItem(root, "offset");
	if (cJSON_IsNumber(off))
		offset_val = (long)off->valuedouble;

	long limit_val = 1000;
	cJSON *lim = cJSON_GetObjectItem(root, "limit");
	if (cJSON_IsNumber(lim) && lim->valuedouble > 0)
		limit_val = (long)lim->valuedouble;
	cJSON *ml = cJSON_GetObjectItem(root, "max_lines");
	if (cJSON_IsNumber(ml) && ml->valuedouble > 0)
		limit_val = (long)ml->valuedouble;

	char *expanded = file_expand_path(file_path);
	const char *resolved = expanded ? expanded : file_path;

	size_t data_len = 0;
	char *data = file_read_all(resolved, &data_len);
	free(expanded);

	if (!data) {
		cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"file not found or cannot be read\"}");
		return -ENOENT;
	}

	int binary = is_binary(data, data_len);

	cJSON *out = cJSON_CreateObject();
	cJSON_AddStringToObject(out, "file_path", file_path);
	cJSON_AddNumberToObject(out, "size", (double)data_len);

	if (binary) {
		char preview[256];
		size_t plen = data_len < 250 ? data_len : 250;
		for (size_t i = 0; i < plen; i++) {
			unsigned char c = (unsigned char)data[i];
			if (c < 32 && c != '\n' && c != '\r' && c != '\t')
				preview[i] = '.';
			else
				preview[i] = (char)c;
		}
		preview[plen] = '\0';
		cJSON_AddBoolToObject(out, "binary", 1);
		cJSON_AddStringToObject(out, "preview", preview);
		cJSON_AddNumberToObject(out, "total_lines", 0);
		cJSON_AddNumberToObject(out, "returned_lines", 0);
		char *str = cJSON_PrintUnformatted(out);
		cJSON_Delete(out);
		free(data);
		cJSON_Delete(root);
		*result_json = str;
		return 0;
	}

	cJSON_AddBoolToObject(out, "binary", 0);

	long total_lines = 0;
	for (const char *p = data; *p; p++) {
		if (*p == '\n')
			total_lines++;
	}
	if (total_lines == 0 && data_len > 0)
		total_lines = 1;
	if (data_len > 0 && data[data_len - 1] != '\n')
		total_lines++;

	cJSON_AddNumberToObject(out, "total_lines", (double)total_lines);

	if (offset_val >= total_lines) {
		cJSON_AddStringToObject(out, "content", "");
		cJSON_AddNumberToObject(out, "returned_lines", 0);
		cJSON_AddBoolToObject(out, "truncated", 0);
		char *str = cJSON_PrintUnformatted(out);
		cJSON_Delete(out);
		free(data);
		cJSON_Delete(root);
		*result_json = str;
		return 0;
	}

	struct line_buffer buf = {0};
	long line_idx = 0;
	const char *p = data;
	int truncated = 0;
	int returned = 0;

	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t llen = nl ? (size_t)(nl - p) : strlen(p);
		if (llen > MAX_LINE_LENGTH)
			llen = MAX_LINE_LENGTH;

		if (line_idx >= offset_val) {
			if (buf_append(&buf, p, llen) < 0) {
				truncated = 1;
				break;
			}
			if (buf_append(&buf, "\n", 1) < 0) {
				truncated = 1;
				break;
			}
			returned++;
			if (returned >= limit_val) {
				if (nl) {
					line_idx++;
					p = nl + 1;
					if (*p)
						truncated = 1;
				}
				break;
			}
		}

		line_idx++;
		if (nl)
			p = nl + 1;
		else
			break;
	}

	cJSON_AddStringToObject(out, "content", buf.data ? buf.data : "");
	cJSON_AddNumberToObject(out, "returned_lines", returned);
	cJSON_AddBoolToObject(out, "truncated", truncated);

	char *str = cJSON_PrintUnformatted(out);
	cJSON_Delete(out);
	free(buf.data);
	free(data);
	cJSON_Delete(root);
	*result_json = str;
	return 0;
}

int file_read_init(struct tool_registry *reg)
{
	if (!reg) return -EINVAL;
	return tool_register(reg, "file_read",
		"Read a text file's content. Provide file_path, optional offset (line number, 0-indexed, to start from), and limit/max_lines (max lines to return, default 1000). Binary files return a short hex preview instead.",
		"{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\",\"description\":\"Path to the text file to read\"},\"offset\":{\"type\":\"integer\",\"description\":\"Line number to start from (0-indexed)\"},\"limit\":{\"type\":\"integer\",\"description\":\"Max lines to return\"},\"max_lines\":{\"type\":\"integer\",\"description\":\"Max lines to return (alternative to limit)\"}},\"required\":[\"file_path\"]}",
		file_read_exec, NULL);
}
