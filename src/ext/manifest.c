#include "manifest.h"
#include "util/log.h"
#include "util/error.h"
#include "toml.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void parse_string_list(toml_array_t *arr, char ***out_list, int *out_count)
{
	*out_list = NULL;
	*out_count = 0;
	if (!arr)
		return;
	int n = toml_array_nelem(arr);
	if (n <= 0)
		return;
	char **list = calloc((size_t)n, sizeof(char *));
	if (!list)
		return;
	int count = 0;
	for (int i = 0; i < n; i++) {
		toml_datum_t d = toml_string_at(arr, i);
		if (d.ok) {
			list[count++] = d.u.s;
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

static int manifest_parse_table(toml_table_t *tbl, struct ext_manifest *out)
{
	if (!tbl || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));

#define MGET_STR(key, buf) do { \
	toml_datum_t _d = toml_string_in(tbl, key); \
	if (_d.ok) { strncpy(buf, _d.u.s, sizeof(buf) - 1); free(_d.u.s); } \
} while (0)

#define MGET_INT(key, var) do { \
	toml_datum_t _d = toml_int_in(tbl, key); \
	if (_d.ok) var = (int)_d.u.i; \
} while (0)

#define MGET_UINT(key, var) do { \
	toml_datum_t _d = toml_int_in(tbl, key); \
	if (_d.ok) var = (unsigned int)_d.u.i; \
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
		toml_datum_t pd = toml_string_in(tbl, "purpose");
		out->purpose = parse_purpose(pd.ok ? pd.u.s : NULL);
		if (pd.ok) free(pd.u.s);
	}

	MGET_STR("hook", out->hook);
	MGET_STR("action_text", out->action_text);

	MGET_UINT("permissions", out->permissions);
	MGET_INT("max_memory_mb", out->max_memory_mb);
	MGET_INT("max_cpu_seconds", out->max_cpu_seconds);
	MGET_INT("max_open_files", out->max_open_files);

	toml_datum_t as = toml_string_in(tbl, "args_schema");
	if (as.ok) out->args_schema = as.u.s; else as.u.s = NULL;

	toml_datum_t os = toml_string_in(tbl, "output_schema");
	if (os.ok) out->output_schema = os.u.s; else os.u.s = NULL;

	toml_array_t *fronts = toml_array_in(tbl, "fronts");
	parse_string_list(fronts, &out->fronts, &out->fronts_count);

	toml_array_t *cats = toml_array_in(tbl, "categories");
	parse_string_list(cats, &out->categories, &out->categories_count);

	toml_table_t *build = toml_table_in(tbl, "build");
	if (build) {
		toml_datum_t cmd = toml_string_in(build, "command");
		if (cmd.ok) {
			strncpy(out->build_command, cmd.u.s,
				sizeof(out->build_command) - 1);
			free(cmd.u.s);
		}
	}

	toml_array_t *ap = toml_array_in(tbl, "allowed_paths");
	parse_string_list(ap, &out->allowed_paths, &out->allowed_paths_count);

	toml_array_t *ae = toml_array_in(tbl, "allowed_env");
	parse_string_list(ae, &out->allowed_env, &out->allowed_env_count);

	log_info("manifest parsed: name=%s type=%s purpose=%d",
		 out->name, out->type, out->purpose);
	return 0;
}

int manifest_parse(const char *toml_data, struct ext_manifest *out)
{
	if (!toml_data || !out)
		return -EINVAL;

	char *copy = strdup(toml_data);
	if (!copy)
		return -ENOMEM;

	char errbuf[256];
	toml_table_t *tbl = toml_parse(copy, errbuf, sizeof(errbuf));
	if (!tbl) {
		log_err("manifest parse error: %s", errbuf);
		free(copy);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}

	int rc = manifest_parse_table(tbl, out);
	toml_free(tbl);
	free(copy);
	return rc;
}

int manifest_parse_file(const char *path, struct ext_manifest *out)
{
	if (!path || !out)
		return -EINVAL;
	FILE *f = fopen(path, "r");
	if (!f)
		return -ENOENT;

	char errbuf[256];
	toml_table_t *tbl = toml_parse_file(f, errbuf, sizeof(errbuf));
	fclose(f);

	if (!tbl) {
		log_err("manifest parse error in %s: %s", path, errbuf);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}

	int rc = manifest_parse_table(tbl, out);
	toml_free(tbl);
	return rc;
}
