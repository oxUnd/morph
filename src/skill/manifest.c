#include "manifest.h"
#include "util/log.h"
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

int manifest_parse(const char *toml_data, struct skill_manifest *out)
{
	if (!toml_data || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));

	char *copy = strdup(toml_data);
	if (!copy)
		return -ENOMEM;

	char errbuf[256];
	toml_table_t *tbl = toml_parse(copy, errbuf, sizeof(errbuf));
	if (!tbl) {
		log_err("manifest parse error: %s", errbuf);
		free(copy);
		return -EIO;
	}

#define MGET_STR(key, buf) do { \
	toml_datum_t _d = toml_string_in(tbl, key); \
	if (_d.ok) { strncpy(buf, _d.u.s, sizeof(buf) - 1); free(_d.u.s); } \
} while(0)

#define MGET_INT(key, var) do { \
	toml_datum_t _d = toml_int_in(tbl, key); \
	if (_d.ok) var = (int)_d.u.i; \
} while(0)

#define MGET_UINT(key, var) do { \
	toml_datum_t _d = toml_int_in(tbl, key); \
	if (_d.ok) var = (unsigned int)_d.u.i; \
} while(0)

	MGET_STR("name", out->name);
	MGET_STR("version", out->version);
	MGET_STR("description", out->description);
	MGET_STR("author", out->author);
	MGET_STR("type", out->type);
	MGET_STR("entry", out->entry);

	MGET_UINT("permissions", out->permissions);
	MGET_INT("max_memory_mb", out->max_memory_mb);
	MGET_INT("max_cpu_seconds", out->max_cpu_seconds);

	toml_datum_t as = toml_string_in(tbl, "args_schema");
	if (as.ok) out->args_schema = as.u.s; else as.u.s = NULL;

	toml_datum_t os = toml_string_in(tbl, "output_schema");
	if (os.ok) out->output_schema = os.u.s; else os.u.s = NULL;

	toml_array_t *ap = toml_array_in(tbl, "allowed_paths");
	parse_string_list(ap, &out->allowed_paths, &out->allowed_paths_count);

	toml_array_t *ae = toml_array_in(tbl, "allowed_env");
	parse_string_list(ae, &out->allowed_env, &out->allowed_env_count);

	toml_free(tbl);
	free(copy);
	log_info("manifest parsed: name=%s type=%s", out->name, out->type);
	return 0;
}

int manifest_parse_file(const char *path, struct skill_manifest *out)
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
		return -EIO;
	}

	memset(out, 0, sizeof(*out));

#define MFGET_STR(key, buf) do { \
	toml_datum_t _d = toml_string_in(tbl, key); \
	if (_d.ok) { strncpy(buf, _d.u.s, sizeof(buf) - 1); free(_d.u.s); } \
} while(0)

#define MFGET_INT(key, var) do { \
	toml_datum_t _d = toml_int_in(tbl, key); \
	if (_d.ok) var = (int)_d.u.i; \
} while(0)

#define MFGET_UINT(key, var) do { \
	toml_datum_t _d = toml_int_in(tbl, key); \
	if (_d.ok) var = (unsigned int)_d.u.i; \
} while(0)

	MFGET_STR("name", out->name);
	MFGET_STR("version", out->version);
	MFGET_STR("description", out->description);
	MFGET_STR("author", out->author);
	MFGET_STR("type", out->type);
	MFGET_STR("entry", out->entry);

	MFGET_UINT("permissions", out->permissions);
	MFGET_INT("max_memory_mb", out->max_memory_mb);
	MFGET_INT("max_cpu_seconds", out->max_cpu_seconds);

	toml_datum_t as = toml_string_in(tbl, "args_schema");
	if (as.ok) out->args_schema = as.u.s; else as.u.s = NULL;

	toml_datum_t os = toml_string_in(tbl, "output_schema");
	if (os.ok) out->output_schema = os.u.s; else os.u.s = NULL;

	toml_array_t *ap = toml_array_in(tbl, "allowed_paths");
	parse_string_list(ap, &out->allowed_paths, &out->allowed_paths_count);

	toml_array_t *ae = toml_array_in(tbl, "allowed_env");
	parse_string_list(ae, &out->allowed_env, &out->allowed_env_count);

	toml_free(tbl);
	log_info("manifest parsed: name=%s type=%s", out->name, out->type);
	return 0;
}