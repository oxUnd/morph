#include "manifest.h"
#include "util/log.h"
#include "util/error.h"
#include "util/cleanup.h"
#include "util/utf8.h"
#include <limits.h>
#include "tomlc17.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int manifest_string_valid(toml_datum_t value)
{
	if (value.type != TOML_STRING || value.u.str.len < 0)
		return -EINVAL;
	size_t len = (size_t)value.u.str.len;
	if (memchr(value.u.s, '\0', len) || utf8nvalid(value.u.s, len))
		return -EILSEQ;
	return 0;
}

static int manifest_string_copy(struct morph_log *logger, toml_datum_t value, const char *key,
				char *destination, size_t capacity)
{
	int rc;

	if (value.type == TOML_UNKNOWN)
		return 0;
	rc = manifest_string_valid(value);
	if (rc == 0 && (size_t)value.u.str.len >= capacity)
		rc = -E2BIG;
	if (rc != 0) {
		log_err(logger, "manifest field '%s' must be valid UTF-8 within %zu bytes: %s",
			key, capacity - 1, morph_strerror(rc));
		return rc;
	}
	memcpy(destination, value.u.s, (size_t)value.u.str.len + 1);
	return 0;
}

static int parse_string_list(toml_datum_t arr, char ***out_list, int *out_count)
{
	if (arr.type != TOML_ARRAY || arr.u.arr.size <= 0)
		return 0;
	int n = arr.u.arr.size;
	char **list = calloc((size_t)n, sizeof(*list));
	if (!list)
		return -ENOMEM;
	*out_list = list;
	*out_count = 0;
	for (int i = 0; i < n; i++) {
		toml_datum_t value = arr.u.arr.elem[i];
		int rc = manifest_string_valid(value);
		if (rc != 0)
			return rc;
		list[*out_count] = strdup(value.u.s);
		if (!list[*out_count])
			return -ENOMEM;
		(*out_count)++;
	}
	return 0;
}

static int manifest_output_occupied(const struct ext_manifest *out)
{
	return out->input_schema || out->output_schema || out->fronts || out->categories ||
	       out->allowed_paths || out->allowed_env || out->allowed_mach_services;
}

static enum ext_purpose parse_purpose(const char *s)
{
	if (!s) return EXT_PURPOSE_TOOL;
	if (strcmp(s, "guardrail") == 0) return EXT_PURPOSE_GUARDRAIL;
	return EXT_PURPOSE_TOOL;
}

static int parse_sandbox_capabilities(struct morph_log *logger, toml_datum_t arr,
				      unsigned int *permissions, int *named_filesystem)
{
	static const struct {
		const char *name;
		unsigned int permission;
	} capabilities[] = {
		{ "network", EXT_PERM_NETWORK },
		{ "filesystem", EXT_PERM_FILESYS },
		{ "exec", EXT_PERM_EXEC },
		{ "environment", EXT_PERM_ENV },
		{ "pty", EXT_PERM_PTY },
		{ "process_info", EXT_PERM_PROCESS_INFO },
		{ "ipc", EXT_PERM_IPC },
		{ "temporary_directory", EXT_PERM_TEMP },
	};

	if (arr.type == TOML_UNKNOWN)
		return 0;
	if (arr.type != TOML_ARRAY || !permissions)
		return -EINVAL;
	for (int i = 0; i < arr.u.arr.size; i++) {
		toml_datum_t item = arr.u.arr.elem[i];
		int found = 0;

		if (item.type != TOML_STRING)
			return -EINVAL;
		for (size_t j = 0; j < sizeof(capabilities) /
					      sizeof(capabilities[0]); j++) {
			if (strcmp(item.u.s, capabilities[j].name) == 0) {
				*permissions |= capabilities[j].permission;
				if (named_filesystem &&
				    capabilities[j].permission == EXT_PERM_FILESYS)
					*named_filesystem = 1;
				found = 1;
				break;
			}
		}
		if (!found) {
			log_err(logger, "unknown sandbox capability: %s", item.u.s);
			return -EINVAL;
		}
	}
	return 0;
}

static char *schema_default_input(void)
{
	return strdup("{\"type\":\"object\",\"properties\":{},"
		      "\"additionalProperties\":false}");
}

static char *schema_default_output(void)
{
	return strdup("{\"type\":\"object\",\"properties\":{}}");
}

static char *normalize_manifest_schema_copy(const char *schema, int is_output)
{
	if (!schema || !*schema)
		return is_output ? schema_default_output() : schema_default_input();
	if (strcmp(schema, "object") == 0 || strcmp(schema, "string") == 0 || schema[0] != '{')
		return schema_default_output();
	return strdup(schema);
}

static int manifest_parse_table(struct morph_log *logger, toml_datum_t tbl,
				struct ext_manifest *out)
{
	int named_filesystem = 0;
	int rc;
	if (tbl.type != TOML_TABLE || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));

#define MGET_INT(key, var) do { \
	toml_datum_t _d = toml_get(tbl, key); \
	if (_d.type == TOML_INT64) var = (int)_d.u.int64; \
} while (0)

#define MGET_UINT(key, var) do { \
	toml_datum_t _d = toml_get(tbl, key); \
	if (_d.type == TOML_INT64) var = (unsigned int)_d.u.int64; \
} while (0)

	struct manifest_string_slot {
		const char *key;
		char *destination;
		size_t capacity;
	} strings[] = {
		{"name", out->name, sizeof(out->name)},
		{"version", out->version, sizeof(out->version)},
		{"description", out->description, sizeof(out->description)},
		{"author", out->author, sizeof(out->author)},
		{"type", out->type, sizeof(out->type)},
		{"entry", out->entry, sizeof(out->entry)},
		{"hook", out->hook, sizeof(out->hook)},
		{"action_text", out->action_text, sizeof(out->action_text)},
	};
	for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
		rc = manifest_string_copy(logger, toml_get(tbl, strings[i].key), strings[i].key,
					  strings[i].destination, strings[i].capacity);
		if (rc != 0)
			return rc;
	}
	const char *resource_keys[] = {"max_memory_mb", "max_cpu_seconds", "max_open_files",
				       "permissions"};
	for (size_t i = 0; i < sizeof(resource_keys) / sizeof(resource_keys[0]); i++) {
		toml_datum_t value = toml_get(tbl, resource_keys[i]);
		int64_t maximum = i == 3 ? UINT_MAX : INT_MAX;
		if (value.type != TOML_UNKNOWN &&
		    (value.type != TOML_INT64 || value.u.int64 < 0 || value.u.int64 > maximum)) {
			log_err(logger, "manifest field '%s' is outside its supported "
				"integer range",
				resource_keys[i]);
			return -ERANGE;
		}
	}
	if (!out->type[0])
		strncpy(out->type, "exec", sizeof(out->type) - 1);

	{
		toml_datum_t pd = toml_get(tbl, "purpose");
		out->purpose = parse_purpose(pd.type == TOML_STRING ? pd.u.s : NULL);
	}

	MGET_UINT("permissions", out->permissions);
	{
		toml_datum_t capabilities = toml_get(tbl,
			"sandbox_capabilities");
		int capability_rc = parse_sandbox_capabilities(
			logger, capabilities, &out->permissions, &named_filesystem);

		if (capability_rc != 0)
			return capability_rc;
	}
	toml_datum_t ap = toml_get(tbl, "allowed_paths");
	rc = parse_string_list(ap, &out->allowed_paths, &out->allowed_paths_count);
	if (rc != 0)
		return rc;
	if (named_filesystem && out->allowed_paths_count == 0) {
		log_err(logger, "filesystem capability requires allowed_paths");
		return -EINVAL;
	}
	MGET_INT("max_memory_mb", out->max_memory_mb);
	MGET_INT("max_cpu_seconds", out->max_cpu_seconds);
	MGET_INT("max_open_files", out->max_open_files);

	toml_datum_t is = toml_get(tbl, "input_schema");
	if (is.type != TOML_STRING)
		is = toml_get(tbl, "args_schema");
	if (is.type == TOML_STRING) {
		rc = manifest_string_valid(is);
		if (rc != 0)
			return rc;
		out->input_schema = normalize_manifest_schema_copy(is.u.s, 0);
	} else
		out->input_schema = schema_default_input();

	toml_datum_t os = toml_get(tbl, "output_schema");
	if (os.type == TOML_STRING) {
		rc = manifest_string_valid(os);
		if (rc != 0)
			return rc;
		out->output_schema = normalize_manifest_schema_copy(os.u.s, 1);
	} else
		out->output_schema = schema_default_output();

	if (!out->input_schema || !out->output_schema)
		return -ENOMEM;
	toml_datum_t fronts = toml_get(tbl, "fronts");
	rc = parse_string_list(fronts, &out->fronts, &out->fronts_count);
	if (rc != 0)
		return rc;

	toml_datum_t cats = toml_get(tbl, "categories");
	rc = parse_string_list(cats, &out->categories, &out->categories_count);
	if (rc != 0)
		return rc;

	toml_datum_t build = toml_get(tbl, "build");
	if (build.type == TOML_TABLE) {
		toml_datum_t cmd = toml_get(build, "command");
		rc = manifest_string_copy(logger, cmd, "build.command", out->build_command,
					  sizeof(out->build_command));
		if (rc != 0)
			return rc;
	}

	toml_datum_t ae = toml_get(tbl, "allowed_env");
	rc = parse_string_list(ae, &out->allowed_env, &out->allowed_env_count);
	if (rc != 0)
		return rc;

	toml_datum_t am = toml_get(tbl, "allowed_mach_services");
	rc = parse_string_list(am, &out->allowed_mach_services, &out->allowed_mach_services_count);
	if (rc != 0)
		return rc;

	log_info(logger, "manifest parsed: name=%s type=%s purpose=%d", out->name, out->type,
		 out->purpose);
	return 0;
}

int manifest_parse(struct morph_log *logger, const char *toml_data, struct ext_manifest *out)
{
	if (!toml_data || !out)
		return -EINVAL;
	if (manifest_output_occupied(out))
		return -EEXIST;
	if (strlen(toml_data) > INT_MAX)
		return -EFBIG;
	struct ext_manifest manifest MORPH_CLEANUP(ext_manifest_cleanup) = {0};
	toml_result_t parsed = toml_parse(toml_data, (int)strlen(toml_data));
	if (!parsed.ok) {
		log_err(logger, "manifest parse error: %s", parsed.errmsg);
		toml_free(parsed);
		return MORPH_ERR_PARSE;
	}

	int rc = manifest_parse_table(logger, parsed.toptab, &manifest);
	toml_free(parsed);
	if (rc == 0) {
		*out = manifest;
		memset(&manifest, 0, sizeof(manifest));
	}
	return rc;
}

int manifest_parse_file(struct morph_log *logger, const char *path, struct ext_manifest *out)
{
	FILE *file;
	toml_result_t parsed;
	int rc;

	if (!path || !out)
		return -EINVAL;
	if (manifest_output_occupied(out))
		return -EEXIST;
	struct ext_manifest manifest MORPH_CLEANUP(ext_manifest_cleanup) = {0};
	file = fopen(path, "r");
	if (!file)
		return morph_error_from_errno(errno);
	parsed = toml_parse_file(file);
	fclose(file);
	if (!parsed.ok) {
		log_err(logger, "manifest parse error in %s: %s", path, parsed.errmsg);
		toml_free(parsed);
		return MORPH_ERR_PARSE;
	}

	rc = manifest_parse_table(logger, parsed.toptab, &manifest);
	toml_free(parsed);
	if (rc == 0) {
		*out = manifest;
		memset(&manifest, 0, sizeof(manifest));
	}
	return rc;
}
