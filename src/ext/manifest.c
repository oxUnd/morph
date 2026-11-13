#include "manifest.h"
#include "nb/log.h"
#include "errors.h"
#include "nb/cleanup.h"
#include "nb/utf8.h"
#include <limits.h>
#include "nb/toml.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int manifest_string_valid(const nb_toml_value *value)
{
	if (nb_toml_type_of(value) != NB_TOML_STRING || false)
		return NB_ERR_INVAL;
	size_t len = (size_t)nb_toml_string(value).len;
	if (memchr(nb_toml_string(value).ptr, '\0', len) ||
	    nb_utf8_validate(nb_str_from_parts(nb_toml_string(value).ptr, len), NULL))
		return NB_ERR_ENCODING;
	return 0;
}

static int manifest_string_copy(struct nb_log *logger, const nb_toml_value *value, const char *key,
				char *destination, size_t capacity)
{
	int rc;

	if (nb_toml_type_of(value) == NB_TOML_NONE)
		return 0;
	rc = manifest_string_valid(value);
	if (rc == 0 && (size_t)nb_toml_string(value).len >= capacity)
		rc = NB_ERR_TOO_BIG;
	if (rc != 0) {
		nb_log_write(logger, NB_LOG_ERROR,
			     "manifest field '%s' must be valid UTF-8 within %zu bytes: %s", key,
			     capacity - 1,
			     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	memcpy(destination, nb_toml_string(value).ptr, (size_t)nb_toml_string(value).len + 1);
	return 0;
}

static int parse_string_list(const nb_toml_value *arr, char ***out_list, int *out_count)
{
	if (nb_toml_type_of(arr) != NB_TOML_ARRAY || nb_toml_array_size(arr) <= 0)
		return 0;
	size_t n = nb_toml_array_size(arr);
	char **list = calloc((size_t)n, sizeof(*list));
	if (!list)
		return NB_ERR_NOMEM;
	*out_list = list;
	*out_count = 0;
	for (size_t i = 0; i < n; i++) {
		const nb_toml_value *value = nb_toml_array_get(arr, (size_t)(i));
		int rc = manifest_string_valid(value);
		if (rc != 0)
			return rc;
		list[*out_count] = strdup(nb_toml_string(value).ptr);
		if (!list[*out_count])
			return NB_ERR_NOMEM;
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

static int parse_sandbox_capabilities(struct nb_log *logger, const nb_toml_value *arr,
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

	if (nb_toml_type_of(arr) == NB_TOML_NONE)
		return 0;
	if (nb_toml_type_of(arr) != NB_TOML_ARRAY || !permissions)
		return NB_ERR_INVAL;
	for (size_t i = 0; i < nb_toml_array_size(arr); i++) {
		const nb_toml_value *item = nb_toml_array_get(arr, (size_t)(i));
		int found = 0;

		if (nb_toml_type_of(item) != NB_TOML_STRING)
			return NB_ERR_INVAL;
		int string_rc = manifest_string_valid(item);
		if (string_rc != NB_OK)
			return string_rc;
		for (size_t j = 0; j < sizeof(capabilities) /
					      sizeof(capabilities[0]); j++) {
			if (strcmp(nb_toml_string(item).ptr, capabilities[j].name) == 0) {
				*permissions |= capabilities[j].permission;
				if (named_filesystem &&
				    capabilities[j].permission == EXT_PERM_FILESYS)
					*named_filesystem = 1;
				found = 1;
				break;
			}
		}
		if (!found) {
			nb_log_write(logger, NB_LOG_ERROR, "unknown sandbox capability: %s",
				     nb_toml_string(item).ptr);
			return NB_ERR_INVAL;
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

static int manifest_parse_table(struct nb_log *logger, const nb_toml_value *tbl,
				struct ext_manifest *out)
{
	int named_filesystem = 0;
	int rc;
	if (nb_toml_type_of(tbl) != NB_TOML_TABLE || !out)
		return NB_ERR_INVAL;
	memset(out, 0, sizeof(*out));

#define MGET_INT(key, var)                                                                         \
	do {                                                                                       \
		const nb_toml_value *_d = nb_toml_table_get_cstr(tbl, key);                        \
		if (nb_toml_type_of(_d) == NB_TOML_INTEGER)                                        \
			var = (int)nb_toml_integer(_d);                                            \
	} while (0)

#define MGET_UINT(key, var)                                                                        \
	do {                                                                                       \
		const nb_toml_value *_d = nb_toml_table_get_cstr(tbl, key);                        \
		if (nb_toml_type_of(_d) == NB_TOML_INTEGER)                                        \
			var = (unsigned int)nb_toml_integer(_d);                                   \
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
		rc = manifest_string_copy(logger, nb_toml_table_get_cstr(tbl, strings[i].key),
					  strings[i].key, strings[i].destination,
					  strings[i].capacity);
		if (rc != 0)
			return rc;
	}
	const char *resource_keys[] = {"max_memory_mb", "max_cpu_seconds", "max_open_files",
				       "permissions"};
	for (size_t i = 0; i < sizeof(resource_keys) / sizeof(resource_keys[0]); i++) {
		const nb_toml_value *value = nb_toml_table_get_cstr(tbl, resource_keys[i]);
		int64_t maximum = i == 3 ? UINT_MAX : INT_MAX;
		if (nb_toml_type_of(value) != NB_TOML_NONE &&
		    (nb_toml_type_of(value) != NB_TOML_INTEGER || nb_toml_integer(value) < 0 ||
		     nb_toml_integer(value) > maximum)) {
			nb_log_write(logger, NB_LOG_ERROR,
				     "manifest field '%s' is outside its supported "
				     "integer range",
				     resource_keys[i]);
			return NB_ERR_RANGE;
		}
	}
	if (!out->type[0])
		strncpy(out->type, "exec", sizeof(out->type) - 1);

	{
		const nb_toml_value *pd = nb_toml_table_get_cstr(tbl, "purpose");
		if (nb_toml_type_of(pd) == NB_TOML_STRING) {
			rc = manifest_string_valid(pd);
			if (rc != NB_OK)
				return rc;
		}
		out->purpose = parse_purpose(
			nb_toml_type_of(pd) == NB_TOML_STRING ? nb_toml_string(pd).ptr : NULL);
	}

	MGET_UINT("permissions", out->permissions);
	{
		const nb_toml_value *capabilities =
			nb_toml_table_get_cstr(tbl, "sandbox_capabilities");
		int capability_rc = parse_sandbox_capabilities(
			logger, capabilities, &out->permissions, &named_filesystem);

		if (capability_rc != 0)
			return capability_rc;
	}
	const nb_toml_value *ap = nb_toml_table_get_cstr(tbl, "allowed_paths");
	rc = parse_string_list(ap, &out->allowed_paths, &out->allowed_paths_count);
	if (rc != 0)
		return rc;
	if (named_filesystem && out->allowed_paths_count == 0) {
		nb_log_write(logger, NB_LOG_ERROR, "filesystem capability requires allowed_paths");
		return NB_ERR_INVAL;
	}
	MGET_INT("max_memory_mb", out->max_memory_mb);
	MGET_INT("max_cpu_seconds", out->max_cpu_seconds);
	MGET_INT("max_open_files", out->max_open_files);

	const nb_toml_value *is = nb_toml_table_get_cstr(tbl, "input_schema");
	if (nb_toml_type_of(is) != NB_TOML_STRING)
		is = nb_toml_table_get_cstr(tbl, "args_schema");
	if (nb_toml_type_of(is) == NB_TOML_STRING) {
		rc = manifest_string_valid(is);
		if (rc != 0)
			return rc;
		out->input_schema = normalize_manifest_schema_copy(nb_toml_string(is).ptr, 0);
	} else
		out->input_schema = schema_default_input();

	const nb_toml_value *os = nb_toml_table_get_cstr(tbl, "output_schema");
	if (nb_toml_type_of(os) == NB_TOML_STRING) {
		rc = manifest_string_valid(os);
		if (rc != 0)
			return rc;
		out->output_schema = normalize_manifest_schema_copy(nb_toml_string(os).ptr, 1);
	} else
		out->output_schema = schema_default_output();

	if (!out->input_schema || !out->output_schema)
		return NB_ERR_NOMEM;
	const nb_toml_value *fronts = nb_toml_table_get_cstr(tbl, "fronts");
	rc = parse_string_list(fronts, &out->fronts, &out->fronts_count);
	if (rc != 0)
		return rc;

	const nb_toml_value *cats = nb_toml_table_get_cstr(tbl, "categories");
	rc = parse_string_list(cats, &out->categories, &out->categories_count);
	if (rc != 0)
		return rc;

	const nb_toml_value *build = nb_toml_table_get_cstr(tbl, "build");
	if (nb_toml_type_of(build) == NB_TOML_TABLE) {
		const nb_toml_value *cmd = nb_toml_table_get_cstr(build, "command");
		rc = manifest_string_copy(logger, cmd, "build.command", out->build_command,
					  sizeof(out->build_command));
		if (rc != 0)
			return rc;
	}

	const nb_toml_value *ae = nb_toml_table_get_cstr(tbl, "allowed_env");
	rc = parse_string_list(ae, &out->allowed_env, &out->allowed_env_count);
	if (rc != 0)
		return rc;

	const nb_toml_value *am = nb_toml_table_get_cstr(tbl, "allowed_mach_services");
	rc = parse_string_list(am, &out->allowed_mach_services, &out->allowed_mach_services_count);
	if (rc != 0)
		return rc;

	nb_log_write(logger, NB_LOG_INFO, "manifest parsed: name=%s type=%s purpose=%d", out->name,
		     out->type, out->purpose);
	return 0;
}

int manifest_parse(struct nb_log *logger, const char *toml_data, struct ext_manifest *out)
{
	nb_err parse_error = {0};
	if (!toml_data || !out)
		return NB_ERR_INVAL;
	if (manifest_output_occupied(out))
		return NB_ERR_EXISTS;
	if (strlen(toml_data) > INT_MAX)
		return NB_ERR_TOO_BIG;
	struct ext_manifest manifest NB_CLEANUP(ext_manifest_cleanup) = {0};
	nb_toml *parsed NB_CLEANUP(nb_toml_cleanup) =
		nb_toml_parse(nb_str_from_cstr(toml_data), &parse_error);
	if (!parsed) {
		nb_log_write(logger, NB_LOG_ERROR, "manifest parse error: %s", parse_error.msg);
		nb_toml_cleanup(&parsed);
		return NB_ERR_PARSE;
	}

	int rc = manifest_parse_table(logger, nb_toml_root(parsed), &manifest);
	nb_toml_cleanup(&parsed);
	if (rc == 0) {
		*out = manifest;
		memset(&manifest, 0, sizeof(manifest));
	}
	return rc;
}

int manifest_parse_file(struct nb_log *logger, const char *path, struct ext_manifest *out)
{
	nb_err parse_error = {0};
	FILE *file;
	nb_toml *parsed NB_CLEANUP(nb_toml_cleanup) = NULL;
	int rc;

	if (!path || !out)
		return NB_ERR_INVAL;
	if (manifest_output_occupied(out))
		return NB_ERR_EXISTS;
	struct ext_manifest manifest NB_CLEANUP(ext_manifest_cleanup) = {0};
	file = fopen(path, "r");
	if (!file)
		return nb_status_from_errno(errno, NULL);
	parsed = nb_toml_load(path, &parse_error);
	fclose(file);
	if (!parsed) {
		nb_log_write(logger, NB_LOG_ERROR, "manifest parse error in %s: %s", path,
			     parse_error.msg);
		nb_toml_cleanup(&parsed);
		return NB_ERR_PARSE;
	}

	rc = manifest_parse_table(logger, nb_toml_root(parsed), &manifest);
	nb_toml_cleanup(&parsed);
	if (rc == 0) {
		*out = manifest;
		memset(&manifest, 0, sizeof(manifest));
	}
	return rc;
}
