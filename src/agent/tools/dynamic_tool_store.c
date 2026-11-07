#include "dynamic_tool_store.h"
#include "util/error.h"
#include "util/file.h"
#include "cJSON.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t fnv1a64(const char *data, size_t len);
static const char *origin_name(enum tool_origin origin);
static int next_checkpoint_id(const char *tool_dir, char *id, size_t id_size);

static int read_definition_text(const char *path, char **text)
{
	errno = 0;
	*text = file_read_all(path, NULL);
	return *text ? 0 : morph_error_from_errno(errno);
}

void dynamic_tool_store_definition_cleanup(struct dynamic_tool_definition *definition)
{
	if (!definition)
		return;
	cJSON_Delete(definition->document);
	free(definition->input_schema);
	free(definition->output_schema);
	memset(definition, 0, sizeof(*definition));
}

static int definition_load_source(struct dynamic_tool_definition *definition,
				  const char *source_path)
{
	const cJSON *field = cJSON_GetObjectItem(definition->document, "source");
	char *legacy = NULL;
	int rc;

	if (field) {
		definition->source = cJSON_GetStringValue(field);
		return definition->source ? 0 : MORPH_ERR_FORMAT;
	}
	rc = read_definition_text(source_path, &legacy);
	if (rc != 0)
		return rc;
	field = cJSON_AddStringToObject(definition->document, "source", legacy);
	free(legacy);
	if (!field)
		return -ENOMEM;
	definition->source = cJSON_GetStringValue(field);
	definition->legacy_source = 1;
	return 0;
}

static char *definition_schema(const cJSON *value, const char *fallback)
{
	if (!value)
		return strdup(fallback);
	if (cJSON_IsString(value))
		return strdup(value->valuestring);
	return cJSON_PrintUnformatted(value);
}

int dynamic_tool_store_load_definition(const char *meta_path, const char *source_path,
				       struct dynamic_tool_definition *definition)
{
	const cJSON *input;
	const cJSON *output;
	char *text = NULL;
	int rc;

	if (!meta_path || !source_path || !definition)
		return -EINVAL;
	memset(definition, 0, sizeof(*definition));
	rc = read_definition_text(meta_path, &text);
	if (rc != 0)
		return rc;
	definition->document = cJSON_ParseWithOpts(text, NULL, 1);
	free(text);
	if (!definition->document)
		return MORPH_ERR_PARSE;
	if (!cJSON_IsObject(definition->document)) {
		dynamic_tool_store_definition_cleanup(definition);
		return MORPH_ERR_FORMAT;
	}
	definition->name = cJSON_GetStringValue(cJSON_GetObjectItem(definition->document, "name"));
	if (!definition->name) {
		dynamic_tool_store_definition_cleanup(definition);
		return MORPH_ERR_FORMAT;
	}
	definition->description =
		cJSON_GetStringValue(cJSON_GetObjectItem(definition->document, "description"));
	rc = definition_load_source(definition, source_path);
	if (rc != 0) {
		dynamic_tool_store_definition_cleanup(definition);
		return rc;
	}
	input = cJSON_GetObjectItem(definition->document, "input_schema");
	output = cJSON_GetObjectItem(definition->document, "output_schema");
	if (cJSON_IsObject(input) && !cJSON_HasObjectItem(input, "properties"))
		input = NULL;
	definition->input_schema = definition_schema(input, TOOL_EMPTY_INPUT_SCHEMA);
	definition->output_schema = definition_schema(output, TOOL_OBJECT_OUTPUT_SCHEMA);
	if (!definition->input_schema || !definition->output_schema) {
		dynamic_tool_store_definition_cleanup(definition);
		return -ENOMEM;
	}
	return 0;
}

int dynamic_tool_store_copy_tool_snapshot(const char *source_meta, const char *source_file,
					  const char *destination_meta,
					  const char *destination_file)
{
	struct dynamic_tool_definition definition;
	char *serialized = NULL;
	int rc = dynamic_tool_store_load_definition(source_meta, source_file, &definition);

	if (rc != 0)
		return rc;
	serialized = cJSON_PrintUnformatted(definition.document);
	if (!serialized)
		rc = -ENOMEM;
	if (rc == 0) {
		rc = file_write_atomic(destination_file, definition.source,
				       strlen(definition.source));
	}
	if (rc == 0) {
		rc = file_write_atomic(destination_meta, serialized, strlen(serialized));
	}
	free(serialized);
	dynamic_tool_store_definition_cleanup(&definition);
	return rc;
}

int dynamic_tool_store_write_tool_files(struct dynamic_tool *dt, const char *source)
{
	cJSON *meta = NULL;
	char *meta_json = NULL;
	char meta_path[PATH_MAX];
	int rc;

	rc = file_ensure_dir(dt->dir);
	if (rc < 0)
		return rc;
	/* Normalize the committed definition before changing its legacy source mirror. */
	struct stat previous;

	rc = file_path_join(meta_path, sizeof(meta_path), dt->dir, DYN_TOOL_META_FILE);
	if (rc != 0)
		return rc;
	if (lstat(meta_path, &previous) == 0) {
		struct dynamic_tool_definition definition;

		rc = dynamic_tool_store_load_definition(meta_path, dt->source_path, &definition);
		if (rc != 0)
			return rc;
		if (definition.legacy_source) {
			char *upgraded = cJSON_PrintUnformatted(definition.document);

			rc = upgraded ? file_write_atomic(meta_path, upgraded, strlen(upgraded))
				      : -ENOMEM;
			free(upgraded);
		}
		dynamic_tool_store_definition_cleanup(&definition);
		if (rc != 0)
			return rc;
	} else if (errno != ENOENT) {
		return morph_error_from_errno(errno);
	}
	char *owned_source = strdup(source);

	if (!owned_source)
		return -ENOMEM;
	free(dt->source);
	dt->source = owned_source;
	source = owned_source;
	rc = file_write_atomic(dt->source_path, source, strlen(source));
	if (rc < 0)
		return rc;
	rc = file_path_join(meta_path, sizeof(meta_path), dt->dir, DYN_TOOL_META_FILE);
	if (rc < 0)
		return rc;
	meta = cJSON_CreateObject();
	if (!meta)
		return -ENOMEM;
	if (!cJSON_AddStringToObject(meta, "source", source)) {
		cJSON_Delete(meta);
		return -ENOMEM;
	}
	if (!cJSON_AddStringToObject(meta, "name", dt->name) ||
	    !cJSON_AddStringToObject(meta, "description", dt->description) ||
	    !cJSON_AddStringToObject(meta, "input_schema", dt->input_schema) ||
	    !cJSON_AddStringToObject(meta, "output_schema", dt->output_schema)) {
		cJSON_Delete(meta);
		return -ENOMEM;
	}
	meta_json = cJSON_PrintUnformatted(meta);
	cJSON_Delete(meta);
	if (!meta_json)
		return -ENOMEM;
	rc = file_write_atomic(meta_path, meta_json, strlen(meta_json));
	free(meta_json);
	return rc;
}

int dynamic_tool_store_tool_file_paths(const char *dir, char *meta_path, size_t meta_size,
				       char *source_path, size_t source_size)
{
	int rc;

	if (!dir)
		return -EINVAL;
	rc = file_path_join(meta_path, meta_size, dir, DYN_TOOL_META_FILE);
	if (rc < 0)
		return rc;
	return file_path_join(source_path, source_size, dir, DYN_TOOL_SOURCE_FILE);
}

int dynamic_tool_store_history_dir_path(const char *tool_dir, char *path, size_t path_size)
{
	return file_path_join(path, path_size, tool_dir, DYN_TOOL_HISTORY_DIR);
}

int dynamic_tool_store_checkpoint_dir_path(const char *tool_dir, const char *checkpoint_id,
					   char *path, size_t path_size)
{
	char history[PATH_MAX];
	int rc;

	rc = dynamic_tool_store_history_dir_path(tool_dir, history, sizeof(history));
	if (rc < 0)
		return rc;
	return file_path_join(path, path_size, history, checkpoint_id);
}

static uint64_t fnv1a64(const char *data, size_t len)
{
	uint64_t hash = 1469598103934665603ULL;

	for (size_t i = 0; i < len; i++) {
		hash ^= (unsigned char)data[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

void dynamic_tool_store_hash_text(const char *data, size_t len, char out[32])
{
	snprintf(out, 32, "%016llx", (unsigned long long)fnv1a64(data ? data : "", len));
}

static const char *origin_name(enum tool_origin origin)
{
	if (origin == TOOL_ORIGIN_DYNAMIC_SESSION)
		return "session";
	if (origin == TOOL_ORIGIN_DYNAMIC_PERSISTENT)
		return "persistent";
	return "unknown";
}

int dynamic_tool_store_checkpoint_id_value(const char *id)
{
	int value = 0;

	if (!id || !*id)
		return -EINVAL;
	for (const char *p = id; *p; p++) {
		if (*p < '0' || *p > '9')
			return -EINVAL;
		value = value * 10 + (*p - '0');
	}
	return value;
}

int dynamic_tool_store_latest_checkpoint_id(const char *tool_dir, char *id, size_t id_size)
{
	char history[PATH_MAX];
	char **dirs = NULL;
	int count = 0;
	int max_id = -1;
	int rc;

	if (!id || id_size == 0)
		return -EINVAL;
	id[0] = '\0';
	rc = dynamic_tool_store_history_dir_path(tool_dir, history, sizeof(history));
	if (rc < 0)
		return rc;
	if (!file_exists(history))
		return -ENOENT;
	if (file_list_dirs(history, &dirs, &count) != 0)
		return -ENOENT;
	for (int i = 0; i < count; i++) {
		int value = dynamic_tool_store_checkpoint_id_value(dirs[i]);
		if (value > max_id)
			max_id = value;
	}
	file_free_list(dirs, count);
	if (max_id < 0)
		return -ENOENT;
	if (snprintf(id, id_size, "%04d", max_id) >= (int)id_size)
		return -ENAMETOOLONG;
	return 0;
}

static int next_checkpoint_id(const char *tool_dir, char *id, size_t id_size)
{
	char latest[16];
	int rc;
	int value = 0;

	rc = dynamic_tool_store_latest_checkpoint_id(tool_dir, latest, sizeof(latest));
	if (rc == 0)
		value = dynamic_tool_store_checkpoint_id_value(latest);
	else if (rc != -ENOENT)
		return rc;
	value++;
	if (snprintf(id, id_size, "%04d", value) >= (int)id_size)
		return -ENAMETOOLONG;
	return 0;
}

int dynamic_tool_store_write_checkpoint_meta(const char *checkpoint_dir,
					     const struct tool_checkpoint *cp)
{
	char path[PATH_MAX];
	cJSON *root;
	char *json;
	int rc;

	rc = file_path_join(path, sizeof(path), checkpoint_dir, DYN_TOOL_CHECKPOINT_META_FILE);
	if (rc < 0)
		return rc;
	root = cJSON_CreateObject();
	if (!root)
		return -ENOMEM;
	cJSON_AddStringToObject(root, "id", cp->id);
	cJSON_AddStringToObject(root, "name", cp->name);
	cJSON_AddStringToObject(root, "before_state", cp->before_state);
	cJSON_AddStringToObject(root, "origin", cp->origin);
	cJSON_AddStringToObject(root, "old_hash", cp->old_hash);
	cJSON_AddStringToObject(root, "new_hash", cp->new_hash);
	cJSON_AddNumberToObject(root, "created_at", (double)cp->created_at);
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json)
		return -ENOMEM;
	rc = file_write_atomic(path, json, strlen(json));
	free(json);
	return rc;
}

int dynamic_tool_store_read_checkpoint_meta(const char *checkpoint_dir, struct tool_checkpoint *cp)
{
	char path[PATH_MAX];
	char *json;
	cJSON *root;
	cJSON *item;
	int rc;

	if (!cp)
		return -EINVAL;
	memset(cp, 0, sizeof(*cp));
	rc = file_path_join(path, sizeof(path), checkpoint_dir, DYN_TOOL_CHECKPOINT_META_FILE);
	if (rc < 0)
		return rc;
	json = file_read_all(path, NULL);
	if (!json)
		return -ENOENT;
	root = cJSON_Parse(json);
	free(json);
	if (!root)
		return -EINVAL;
	item = cJSON_GetObjectItem(root, "id");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(cp->id, item->valuestring, sizeof(cp->id) - 1);
	item = cJSON_GetObjectItem(root, "name");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(cp->name, item->valuestring, sizeof(cp->name) - 1);
	item = cJSON_GetObjectItem(root, "before_state");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(cp->before_state, item->valuestring, sizeof(cp->before_state) - 1);
	item = cJSON_GetObjectItem(root, "origin");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(cp->origin, item->valuestring, sizeof(cp->origin) - 1);
	item = cJSON_GetObjectItem(root, "old_hash");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(cp->old_hash, item->valuestring, sizeof(cp->old_hash) - 1);
	item = cJSON_GetObjectItem(root, "new_hash");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(cp->new_hash, item->valuestring, sizeof(cp->new_hash) - 1);
	item = cJSON_GetObjectItem(root, "created_at");
	if (cJSON_IsNumber(item))
		cp->created_at = (long)item->valuedouble;
	cJSON_Delete(root);
	if (!cp->id[0] || !cp->name[0] || !cp->before_state[0])
		return -EINVAL;
	return 0;
}

int dynamic_tool_store_remove_checkpoint_ids_from(const char *tool_dir, int min_id)
{
	char history[PATH_MAX];
	char **dirs = NULL;
	int count = 0;
	int rc;

	rc = dynamic_tool_store_history_dir_path(tool_dir, history, sizeof(history));
	if (rc < 0)
		return rc;
	if (!file_exists(history))
		return 0;
	if (file_list_dirs(history, &dirs, &count) != 0)
		return 0;
	for (int i = 0; i < count; i++) {
		int value = dynamic_tool_store_checkpoint_id_value(dirs[i]);
		char path[PATH_MAX];

		if (value < min_id)
			continue;
		if (file_path_join(path, sizeof(path), history, dirs[i]) != 0)
			continue;
		(void)dynamic_tool_store_remove_tree(path);
	}
	file_free_list(dirs, count);
	return 0;
}

int dynamic_tool_store_prune_checkpoint_history(const char *tool_dir)
{
	char history[PATH_MAX];
	char **dirs = NULL;
	int count = 0;
	int rc;

	rc = dynamic_tool_store_history_dir_path(tool_dir, history, sizeof(history));
	if (rc < 0)
		return rc;
	if (!file_exists(history))
		return 0;
	if (file_list_dirs(history, &dirs, &count) != 0)
		return 0;
	while (count > DYN_TOOL_HISTORY_LIMIT) {
		int min_idx = -1;
		int min_id = INT_MAX;

		for (int i = 0; i < count; i++) {
			int value = dynamic_tool_store_checkpoint_id_value(dirs[i]);
			if (value >= 0 && value < min_id) {
				min_id = value;
				min_idx = i;
			}
		}
		if (min_idx < 0)
			break;
		{
			char path[PATH_MAX];
			if (file_path_join(path, sizeof(path), history, dirs[min_idx]) == 0)
				(void)dynamic_tool_store_remove_tree(path);
		}
		free(dirs[min_idx]);
		for (int i = min_idx; i < count - 1; i++)
			dirs[i] = dirs[i + 1];
		count--;
	}
	file_free_list(dirs, count);
	return 0;
}

int dynamic_tool_store_create_tool_checkpoint(const char *tool_dir,
					      const struct dynamic_tool *old_dt, const char *name,
					      struct tool_checkpoint *cp, char *checkpoint_dir,
					      size_t checkpoint_dir_size)
{
	char history[PATH_MAX];
	char id[16];
	char old_meta[PATH_MAX];
	char old_source[PATH_MAX];
	char cp_meta[PATH_MAX];
	char cp_source[PATH_MAX];
	char *source = NULL;
	size_t source_len = 0;
	int rc;

	if (!tool_dir || !name || !cp || !checkpoint_dir)
		return -EINVAL;
	memset(cp, 0, sizeof(*cp));
	rc = file_ensure_dir(tool_dir);
	if (rc < 0)
		return rc;
	rc = dynamic_tool_store_history_dir_path(tool_dir, history, sizeof(history));
	if (rc == 0)
		rc = file_ensure_dir(history);
	if (rc == 0)
		rc = next_checkpoint_id(tool_dir, id, sizeof(id));
	if (rc == 0)
		rc = dynamic_tool_store_checkpoint_dir_path(tool_dir, id, checkpoint_dir,
							    checkpoint_dir_size);
	if (rc == 0)
		rc = file_ensure_dir(checkpoint_dir);
	if (rc < 0)
		return rc;
	strncpy(cp->id, id, sizeof(cp->id) - 1);
	strncpy(cp->name, name, sizeof(cp->name) - 1);
	strncpy(cp->before_state, "absent", sizeof(cp->before_state) - 1);
	strncpy(cp->origin, "none", sizeof(cp->origin) - 1);
	strncpy(cp->old_hash, "absent", sizeof(cp->old_hash) - 1);
	cp->created_at = (long)time(NULL);
	if (old_dt) {
		strncpy(cp->before_state, "present", sizeof(cp->before_state) - 1);
		strncpy(cp->origin, origin_name(old_dt->origin), sizeof(cp->origin) - 1);
		rc = dynamic_tool_store_tool_file_paths(old_dt->dir, old_meta, sizeof(old_meta),
							old_source, sizeof(old_source));
		if (rc == 0)
			rc = dynamic_tool_store_tool_file_paths(checkpoint_dir, cp_meta,
								sizeof(cp_meta), cp_source,
								sizeof(cp_source));
		if (rc == 0) {
			rc = dynamic_tool_store_copy_tool_snapshot(old_meta, old_source, cp_meta,
								   cp_source);
		}
		if (rc == 0) {
			source = file_read_all(cp_source, &source_len);
			if (!source)
				rc = -ENOENT;
		}
		if (rc == 0) {
			dynamic_tool_store_hash_text(source, source_len, cp->old_hash);
			free(source);
			source = NULL;
		}
	}
	if (rc == 0)
		rc = dynamic_tool_store_write_checkpoint_meta(checkpoint_dir, cp);
	if (source)
		free(source);
	if (rc < 0)
		(void)dynamic_tool_store_remove_tree(checkpoint_dir);
	return rc;
}

int dynamic_tool_store_restore_tool_checkpoint(const char *tool_dir, const char *checkpoint_dir,
					       const struct tool_checkpoint *cp)
{
	char old_meta[PATH_MAX];
	char old_source[PATH_MAX];
	char dst_meta[PATH_MAX];
	char dst_source[PATH_MAX];
	int rc;

	if (!tool_dir || !checkpoint_dir || !cp)
		return -EINVAL;
	if (strcmp(cp->before_state, "absent") == 0) {
		if (file_exists(tool_dir))
			return dynamic_tool_store_remove_tree(tool_dir);
		return 0;
	}
	if (strcmp(cp->before_state, "present") != 0)
		return -EINVAL;
	rc = file_ensure_dir(tool_dir);
	if (rc == 0)
		rc = dynamic_tool_store_tool_file_paths(checkpoint_dir, old_meta, sizeof(old_meta),
							old_source, sizeof(old_source));
	if (rc == 0)
		rc = dynamic_tool_store_tool_file_paths(tool_dir, dst_meta, sizeof(dst_meta),
							dst_source, sizeof(dst_source));
	if (rc == 0) {
		rc = dynamic_tool_store_copy_tool_snapshot(old_meta, old_source, dst_meta,
							   dst_source);
	}
	return rc;
}

int dynamic_tool_store_remove_tree(const char *path)
{
	struct stat st;
	DIR *dir;
	struct dirent *ent;

	if (!path || !*path)
		return -EINVAL;
	if (lstat(path, &st) != 0)
		return -errno;
	if (!S_ISDIR(st.st_mode)) {
		if (unlink(path) != 0)
			return -errno;
		return 0;
	}
	dir = opendir(path);
	if (!dir)
		return -errno;
	while ((ent = readdir(dir)) != NULL) {
		char child[PATH_MAX];
		int rc;

		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		rc = file_path_join(child, sizeof(child), path, ent->d_name);
		if (rc < 0) {
			closedir(dir);
			return rc;
		}
		rc = dynamic_tool_store_remove_tree(child);
		if (rc < 0) {
			closedir(dir);
			return rc;
		}
	}
	closedir(dir);
	if (rmdir(path) != 0)
		return -errno;
	return 0;
}

char *dynamic_tool_store_read_optional_file(const char *path)
{
	if (!path || !file_exists(path))
		return strdup("");
	return file_read_all(path, NULL);
}

int dynamic_tool_store_append_whole_file_diff(morph_buf_t *buf, const char *label,
					      const char *old_text, const char *new_text)
{
	const char *old_ptr = old_text ? old_text : "";
	const char *new_ptr = new_text ? new_text : "";
	int old_lines = 0;
	int new_lines = 0;
	int rc;

	if (strcmp(old_ptr, new_ptr) == 0)
		return 0;
	for (const char *p = old_ptr; *p; p++)
		if (*p == '\n')
			old_lines++;
	if (*old_ptr)
		old_lines++;
	for (const char *p = new_ptr; *p; p++)
		if (*p == '\n')
			new_lines++;
	if (*new_ptr)
		new_lines++;
	rc = morph_buf_printf(buf, "--- a/%s\n+++ b/%s\n@@ -1,%d +1,%d @@\n", label, label,
			      old_lines, new_lines);
	if (rc != 0)
		return rc;
	for (const char *p = old_ptr; *p;) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) + 1 : strlen(p);

		rc = morph_buf_putc(buf, '-');
		if (rc == 0)
			rc = morph_buf_append(buf, p, len);
		if (rc != 0)
			return rc;
		if (!eol)
			rc = morph_buf_putc(buf, '\n');
		if (rc != 0)
			return rc;
		p += len;
	}
	for (const char *p = new_ptr; *p;) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) + 1 : strlen(p);

		rc = morph_buf_putc(buf, '+');
		if (rc == 0)
			rc = morph_buf_append(buf, p, len);
		if (rc != 0)
			return rc;
		if (!eol)
			rc = morph_buf_putc(buf, '\n');
		if (rc != 0)
			return rc;
		p += len;
	}
	return 0;
}
