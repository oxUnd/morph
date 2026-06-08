#include "file_read.h"
#include "agent/tool_context.h"
#include "util/log.h"
#include "util/file.h"
#include "util/buf.h"
#include "util/error.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#define BINARY_CHECK_SIZE 4096
#define MAX_LINE_LENGTH 65536
#define MAX_CONTENT_SIZE (10 * 1024 * 1024)

static int buf_append(morph_buf_t *b, const char *s, size_t n)
{
	if (!n)
		return 0;
	if (b->len >= MAX_CONTENT_SIZE ||
	    n > MAX_CONTENT_SIZE - b->len - 1)
		MORPH_RETURN(-EFBIG);
	return morph_buf_append(b, s, n);
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

static int file_read_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct tool_context *tctx = user_data;
	if (!result) return -EINVAL;

	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		(void)tool_result_take_text(result, strdup("{\"error\":\"invalid JSON\"}"));
		return -EINVAL;
	}

	cJSON *fp = cJSON_GetObjectItem(root, "file_path");
	const char *file_path = cJSON_IsString(fp) ? fp->valuestring : NULL;
	if (!file_path) {
		cJSON_Delete(root);
		(void)tool_result_take_text(result, strdup(
			"{\"error\":\"missing 'file_path' parameter. "
			"Usage: file_read({\\\"file_path\\\": \\\"path/to/file\\\"})\"}"));
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

	char resolved_path[PATH_MAX];
	if (tctx) {
		int rc = tool_context_authorize_path(tctx, TOOL_PATH_READ,
						     file_path, resolved_path,
						     sizeof(resolved_path));
		if (rc < 0) {
			cJSON_Delete(root);
			if (rc == -ENOENT)
				(void)tool_result_take_text(result, strdup(
					"{\"error\":\"file not found or cannot be read\"}"));
			else
				(void)tool_result_take_text(result, strdup(
					"{\"error\":\"read path outside workspace: permission denied\"}"));
			return rc;
		}
	}
	if (!tctx) {
		strncpy(resolved_path, file_path, sizeof(resolved_path) - 1);
		resolved_path[sizeof(resolved_path) - 1] = '\0';
	}

	size_t data_len = 0;
	char *data = file_read_all(resolved_path, &data_len);

	if (!data) {
		cJSON_Delete(root);
		(void)tool_result_take_text(result, strdup("{\"error\":\"file not found or cannot be read\"}"));
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
		(void)tool_result_take_text(result, str);
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
		(void)tool_result_take_text(result, str);
		return 0;
	}

	morph_buf_t buf;
	int buf_ok = morph_buf_init(&buf, 65536);
	if (buf_ok != 0) {
		cJSON_Delete(out);
		free(data);
		cJSON_Delete(root);
		MORPH_RETURN(buf_ok);
	}
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

	const char *content_str = morph_buf_cstr(&buf);
	cJSON_AddStringToObject(out, "content", content_str ? content_str : "");
	cJSON_AddNumberToObject(out, "returned_lines", returned);
	cJSON_AddBoolToObject(out, "truncated", truncated);

	char *str = cJSON_PrintUnformatted(out);
	cJSON_Delete(out);
	morph_buf_cleanup(&buf);
	free(data);
	cJSON_Delete(root);
	(void)tool_result_take_text(result, str);
	return 0;
}

int file_read_init(struct tool_registry *reg, struct tool_context *tctx)
{
	if (!reg) return -EINVAL;
	return tool_register(reg, "file_read",
		"Read a text file's content. Provide file_path, optional offset (line number, 0-indexed, to start from), and limit/max_lines (max lines to return, default 1000). Binary files return a short hex preview instead.",
		"{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\",\"description\":\"Path to the text file to read\"},\"offset\":{\"type\":\"integer\",\"description\":\"Line number to start from (0-indexed)\"},\"limit\":{\"type\":\"integer\",\"description\":\"Max lines to return\"},\"max_lines\":{\"type\":\"integer\",\"description\":\"Max lines to return (alternative to limit)\"}},\"required\":[\"file_path\"]}",
		file_read_exec, tctx, NULL);
}
