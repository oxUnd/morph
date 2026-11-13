#include "nb/err.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

int ext_run(const char *args_json, char **result_json)
{
	if (!args_json || !result_json)
		return NB_ERR_INVAL;

	const char *key = strstr(args_json, "\"text\"");
	if (!key) {
		*result_json = strdup(
			"{\"error\":\"missing text param\"}");
		return NB_ERR_INVAL;
	}

	const char *val_start = strchr(key + 6, '"');
	if (!val_start) {
		*result_json = strdup(
			"{\"error\":\"malformed text value\"}");
		return NB_ERR_INVAL;
	}
	val_start++;

	size_t val_len = 0;
	const char *p = val_start;
	while (*p && *p != '"') {
		if (*p == '\\')
			p++;
		p++;
		val_len++;
	}

	char *text = malloc(val_len + 1);
	if (!text) {
		*result_json = strdup(
			"{\"error\":\"out of memory\"}");
		return NB_ERR_NOMEM;
	}

	size_t j = 0;
	p = val_start;
	while (*p && *p != '"') {
		if (*p == '\\') {
			p++;
			switch (*p) {
			case 'n': text[j++] = '\n'; break;
			case 't': text[j++] = '\t'; break;
			case '\\': text[j++] = '\\'; break;
			case '"': text[j++] = '"'; break;
			default: text[j++] = *p; break;
			}
			p++;
		} else {
			text[j++] = *p++;
		}
	}
	text[j] = '\0';

	for (size_t i = 0; text[i]; i++)
		text[i] = (char)toupper((unsigned char)text[i]);

	size_t buf_size = j + 64;
	char *buf = malloc(buf_size);
	if (!buf) {
		free(text);
		*result_json = strdup(
			"{\"error\":\"out of memory\"}");
		return NB_ERR_NOMEM;
	}
	snprintf(buf, buf_size,
		 "{\"result\":\"%s\"}", text);
	free(text);

	*result_json = buf;
	return 0;
}