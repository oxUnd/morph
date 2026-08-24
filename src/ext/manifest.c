#include "manifest.h"
#include "util/log.h"
#include "util/error.h"
#include "tomlc17.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void parse_string_list(toml_datum_t arr, char ***out_list, int *out_count)
{
	*out_list = NULL;
	*out_count = 0;
	if (arr.type != TOML_ARRAY)
		return;
	int n = arr.u.arr.size;
	if (n <= 0)
		return;
	char **list = calloc((size_t)n, sizeof(char *));
	if (!list)
		return;
	int count = 0;
	for (int i = 0; i < n; i++) {
		toml_datum_t d = arr.u.arr.elem[i];
		if (d.type == TOML_STRING) {
			list[count] = strdup(d.u.s);
			if (list[count])
				count++;
		}
	}
	*out_list = list;
	*out_count = count;
}

static enum ext_purpose parse_purpose(const char *s)
{
	if (!s) return EXT_PURPOSE_TOOL;
	if (strcmp(s, "guardrail") == 0) return EXT_PURPOSE_GUARDRAIL;
	return EXT_PURPOSE_TOOL;
}

static int parse_sandbox_capabilities(toml_datum_t arr,
				      unsigned int *permissions,
				      int *named_filesystem)
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
		MORPH_RETURN(-EINVAL);
	for (int i = 0; i < arr.u.arr.size; i++) {
		toml_datum_t item = arr.u.arr.elem[i];
		int found = 0;

		if (item.type != TOML_STRING)
			MORPH_RETURN(-EINVAL);
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
			log_err("unknown sandbox capability: %s", item.u.s);
			MORPH_RETURN(-EINVAL);
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

static char *normalize_manifest_schema(char *schema, int is_output)
{
	char *out;

	if (!schema || !*schema) {
		free(schema);
		return is_output ? schema_default_output() :
			schema_default_input();
	}
	if (strcmp(schema, "object") == 0 || strcmp(schema, "string") == 0) {
		free(schema);
		return schema_default_output();
	}
	if (schema[0] == '{')
		return schema;
	out = schema_default_output();
	free(schema);
	return out;
}

static int manifest_parse_table(toml_datum_t tbl, struct ext_manifest *out)
{
	int named_filesystem = 0;
	if (tbl.type != TOML_TABLE || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));

#define MGET_STR(key, buf) do { \
	toml_datum_t _d = toml_get(tbl, key); \
	if (_d.type == TOML_STRING) strncpy(buf, _d.u.s, sizeof(buf) - 1); \
} while (0)

#define MGET_INT(key, var) do { \
	toml_datum_t _d = toml_get(tbl, key); \
	if (_d.type == TOML_INT64) var = (int)_d.u.int64; \
} while (0)

#define MGET_UINT(key, var) do { \
	toml_datum_t _d = toml_get(tbl, key); \
	if (_d.type == TOML_INT64) var = (unsigned int)_d.u.int64; \
} while (0)

	MGET_STR("name", out->name);
	MGET_STR("version", out->version);
	MGET_STR("description", out->description);
	MGET_STR("author", out->author);
	MGET_STR("type", out->type);
	MGET_STR("entry", out->entry);
	if (!out->type[0])
		strncpy(out->type, "exec", sizeof(out->type) - 1);

	{
		toml_datum_t pd = toml_get(tbl, "purpose");
		out->purpose = parse_purpose(pd.type == TOML_STRING ? pd.u.s : NULL);
	}

	MGET_STR("hook", out->hook);
	MGET_STR("action_text", out->action_text);

	MGET_UINT("permissions", out->permissions);
	{
		toml_datum_t capabilities = toml_get(tbl,
			"sandbox_capabilities");
		int capability_rc = parse_sandbox_capabilities(capabilities,
			&out->permissions, &named_filesystem);

		if (capability_rc != 0)
			return capability_rc;
	}
	toml_datum_t ap = toml_get(tbl, "allowed_paths");
	parse_string_list(ap, &out->allowed_paths, &out->allowed_paths_count);
	if (named_filesystem && out->allowed_paths_count == 0) {
		log_err("filesystem capability requires allowed_paths");
		MORPH_RETURN(-EINVAL);
	}
	MGET_INT("max_memory_mb", out->max_memory_mb);
	MGET_INT("max_cpu_seconds", out->max_cpu_seconds);
	MGET_INT("max_open_files", out->max_open_files);

	toml_datum_t is = toml_get(tbl, "input_schema");
	if (is.type != TOML_STRING)
		is = toml_get(tbl, "args_schema");
	if (is.type == TOML_STRING)
		out->input_schema = normalize_manifest_schema(strdup(is.u.s), 0);
	else
		out->input_schema = schema_default_input();

	toml_datum_t os = toml_get(tbl, "output_schema");
	if (os.type == TOML_STRING)
		out->output_schema = normalize_manifest_schema(strdup(os.u.s), 1);
	else
		out->output_schema = schema_default_output();

	toml_datum_t fronts = toml_get(tbl, "fronts");
	parse_string_list(fronts, &out->fronts, &out->fronts_count);

	toml_datum_t cats = toml_get(tbl, "categories");
	parse_string_list(cats, &out->categories, &out->categories_count);

	toml_datum_t build = toml_get(tbl, "build");
	if (build.type == TOML_TABLE) {
		toml_datum_t cmd = toml_get(build, "command");
		if (cmd.type == TOML_STRING) {
			strncpy(out->build_command, cmd.u.s,
				sizeof(out->build_command) - 1);
		}
	}

	toml_datum_t ae = toml_get(tbl, "allowed_env");
	parse_string_list(ae, &out->allowed_env, &out->allowed_env_count);

	toml_datum_t am = toml_get(tbl, "allowed_mach_services");
	parse_string_list(am, &out->allowed_mach_services,
		&out->allowed_mach_services_count);

	log_info("manifest parsed: name=%s type=%s purpose=%d",
		 out->name, out->type, out->purpose);
	return 0;
}

int manifest_parse(const char *toml_data, struct ext_manifest *out)
{
	if (!toml_data || !out)
		return -EINVAL;

	toml_result_t parsed = toml_parse(toml_data, (int)strlen(toml_data));
	if (!parsed.ok) {
		log_err("manifest parse error: %s", parsed.errmsg);
		toml_free(parsed);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}

	int rc = manifest_parse_table(parsed.toptab, out);
	toml_free(parsed);
	return rc;
}

int manifest_parse_file(const char *path, struct ext_manifest *out)
{
	FILE *file;
	toml_result_t parsed;
	int rc;

	if (!path || !out)
		return -EINVAL;
	file = fopen(path, "r");
	if (!file)
		MORPH_RETURN_ERRNO();
	parsed = toml_parse_file(file);
	fclose(file);
	if (!parsed.ok) {
		log_err("manifest parse error in %s: %s", path, parsed.errmsg);
		toml_free(parsed);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}

	rc = manifest_parse_table(parsed.toptab, out);
	toml_free(parsed);
	return rc;
}
