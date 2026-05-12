#include "manifest.h"
#include "util/log.h"
#include "util/file.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *find_value(const char *data, const char *key)
{
	char search[256];
	snprintf(search, sizeof(search), "%s=", key);
	const char *p = strstr(data, search);
	if (!p) {
		snprintf(search, sizeof(search), "%s =", key);
		p = strstr(data, search);
	}
	if (!p)
		return NULL;
	p += strlen(search);
	while (*p == ' ' || *p == '\t')
		p++;
	if ((*p == '"' || *p == '\'')) {
		char quote = *p++;
		const char *end = strchr(p, quote);
		if (!end)
			return NULL;
		size_t len = (size_t)(end - p);
		char *val = malloc(len + 1);
		if (!val)
			return NULL;
		memcpy(val, p, len);
		val[len] = '\0';
		return val;
	}
	const char *end = p;
	while (*end && *end != '\n' && *end != '\r' && *end != ' ' && *end != '\t')
		end++;
	size_t len = (size_t)(end - p);
	char *val = malloc(len + 1);
	if (!val)
		return NULL;
	memcpy(val, p, len);
	val[len] = '\0';
	return val;
}

static char **parse_string_list(const char *data, const char *key, int *count)
{
	*count = 0;
	char search[256];
	snprintf(search, sizeof(search), "%s=[", key);
	const char *p = strstr(data, search);
	if (!p) {
		snprintf(search, sizeof(search), "%s = [", key);
		p = strstr(data, search);
	}
	if (!p)
		return NULL;
	p = strchr(p, '[');
	if (!p)
		return NULL;
	p++;
	int cap = 8;
	char **list = malloc(sizeof(char *) * (size_t)cap);
	if (!list)
		return NULL;
	while (*p && *p != ']') {
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')
			p++;
		if (*p == ']')
			break;
		char quote = '\0';
		if (*p == '"' || *p == '\'')
			quote = *p++;
		const char *start = p;
		if (quote) {
			while (*p && *p != quote)
				p++;
		} else {
			while (*p && *p != ' ' && *p != ',' && *p != ']' &&
			       *p != '\n' && *p != '\r')
				p++;
		}
		size_t len = (size_t)(p - start);
		if (len == 0)
			continue;
		if (*count >= cap) {
			cap *= 2;
			char **new_list = realloc(list, sizeof(char *) * (size_t)cap);
			if (!new_list) {
				for (int i = 0; i < *count; i++)
					free(list[i]);
				free(list);
				return NULL;
			}
			list = new_list;
		}
		list[*count] = malloc(len + 1);
		memcpy(list[*count], start, len);
		list[*count][len] = '\0';
		(*count)++;
		if (quote && *p)
			p++;
	}
	return list;
}

int manifest_parse(const char *toml_data, struct skill_manifest *out)
{
	if (!toml_data || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	char *val;
	val = find_value(toml_data, "name");
	if (val) {
		strncpy(out->name, val, sizeof(out->name) - 1);
		free(val);
	}
	val = find_value(toml_data, "version");
	if (val) {
		strncpy(out->version, val, sizeof(out->version) - 1);
		free(val);
	}
	val = find_value(toml_data, "description");
	if (val) {
		strncpy(out->description, val, sizeof(out->description) - 1);
		free(val);
	}
	val = find_value(toml_data, "author");
	if (val) {
		strncpy(out->author, val, sizeof(out->author) - 1);
		free(val);
	}
	val = find_value(toml_data, "type");
	if (val) {
		strncpy(out->type, val, sizeof(out->type) - 1);
		free(val);
	}
	val = find_value(toml_data, "entry");
	if (val) {
		strncpy(out->entry, val, sizeof(out->entry) - 1);
		free(val);
	}
	val = find_value(toml_data, "permissions");
	if (val) {
		out->permissions = (unsigned int)strtoul(val, NULL, 0);
		free(val);
	}
	val = find_value(toml_data, "max_memory_mb");
	if (val) {
		out->max_memory_mb = atoi(val);
		free(val);
	}
	val = find_value(toml_data, "max_cpu_seconds");
	if (val) {
		out->max_cpu_seconds = atoi(val);
		free(val);
	}
	val = find_value(toml_data, "args_schema");
	if (val)
		out->args_schema = val;
	val = find_value(toml_data, "output_schema");
	if (val)
		out->output_schema = val;
	out->allowed_paths = parse_string_list(toml_data, "allowed_paths",
					       &out->allowed_paths_count);
	out->allowed_env = parse_string_list(toml_data, "allowed_env",
					     &out->allowed_env_count);
	log_info("manifest parsed: name=%s type=%s", out->name, out->type);
	return 0;
}

int manifest_parse_file(const char *path, struct skill_manifest *out)
{
	if (!path || !out)
		return -EINVAL;
	size_t len = 0;
	char *data = file_read_all(path, &len);
	if (!data)
		return -ENOENT;
	int rc = manifest_parse(data, out);
	free(data);
	return rc;
}