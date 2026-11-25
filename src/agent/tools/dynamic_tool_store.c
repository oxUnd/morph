#include "dynamic_tool_store.h"
#include <nb/str.h>
#include <nb/utf8.h>
#include <limits.h>
#include <nb/cleanup.h>
#include "errors.h"
#include <nb/file.h>
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
	nb_buf legacy NB_CLEANUP(nb_buf_destroy) = {0};
	int rc;

	if (field) {
		definition->source = cJSON_GetStringValue(field);
		return definition->source ? 0 : NB_ERR_FORMAT;
	}
	rc = nb_file_read_all(source_path, &legacy, 0, NULL);
	if (rc != 0)
		return rc;
	field = cJSON_AddStringToObject(definition->document, "source", nb_buf_cstr(&legacy));
	if (!field)
		return NB_ERR_NOMEM;
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
	nb_buf text NB_CLEANUP(nb_buf_destroy) = {0};
	int rc;

	if (!meta_path || !source_path || !definition)
		return NB_ERR_INVAL;
	memset(definition, 0, sizeof(*definition));
	rc = nb_file_read_all(meta_path, &text, 0, NULL);
	if (rc != 0)
		return rc;
	definition->document = cJSON_ParseWithOpts(nb_buf_cstr(&text), NULL, 1);
	nb_buf_destroy(&text);
	if (!definition->document)
		return NB_ERR_PARSE;
	if (!cJSON_IsObject(definition->document)) {
		dynamic_tool_store_definition_cleanup(definition);
		return NB_ERR_FORMAT;
	}
	definition->name = cJSON_GetStringValue(cJSON_GetObjectItem(definition->document, "name"));
	if (!definition->name) {
		dynamic_tool_store_definition_cleanup(definition);
		return NB_ERR_FORMAT;
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
		return NB_ERR_NOMEM;
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
		rc = NB_ERR_NOMEM;
	if (rc == 0) {
		rc = nb_file_write_all_atomic(
			destination_file, definition.source, strlen(definition.source),
			&(nb_file_write_options){.new_file_mode = 0600, .sync_dir = true}, NULL);
	}
	if (rc == 0) {
		rc = nb_file_write_all_atomic(
			destination_meta, serialized, strlen(serialized),
			&(nb_file_write_options){.new_file_mode = 0600, .sync_dir = true}, NULL);
	}
	free(serialized);
	dynamic_tool_store_definition_cleanup(&definition);
	return rc;
}

int dynamic_tool_store_write_tool_files(struct dynamic_tool *dt, const char *source)
{
	nb_buf previous_metadata_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int previous_metadata_path_status = NB_OK;
	nb_buf committed_metadata_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int committed_metadata_path_status = NB_OK;

	cJSON *meta = NULL;
	char *meta_json = NULL;
	char meta_path[PATH_MAX];
	int rc;

	rc = nb_file_mkdir(dt->dir, true, NULL);
	if (rc < 0)
		return rc;
	/* Normalize the committed definition before changing its legacy source mirror. */
	struct stat previous;

	rc = ((previous_metadata_path_status = nb_path_concat(&previous_metadata_path_buffer,
							      dt->dir, DYN_TOOL_META_FILE)) != NB_OK
		      ? previous_metadata_path_status
	      : (previous_metadata_path_status =
			 nb_str_copy_to(nb_buf_str(&previous_metadata_path_buffer), meta_path,
					sizeof(meta_path))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : previous_metadata_path_status);
	if (rc != 0)
		return rc;
	if (lstat(meta_path, &previous) == 0) {
		struct dynamic_tool_definition definition;

		rc = dynamic_tool_store_load_definition(meta_path, dt->source_path, &definition);
		if (rc != 0)
			return rc;
		if (definition.legacy_source) {
			char *upgraded = cJSON_PrintUnformatted(definition.document);

			rc = upgraded ? nb_file_write_all_atomic(
						meta_path, upgraded, strlen(upgraded),
						&(nb_file_write_options){.new_file_mode = 0600,
									 .sync_dir = true},
						NULL)
				      : NB_ERR_NOMEM;
			free(upgraded);
		}
		dynamic_tool_store_definition_cleanup(&definition);
		if (rc != 0)
			return rc;
	} else if (errno != ENOENT) {
		return nb_status_from_errno(errno, NULL);
	}
	char *owned_source = strdup(source);

	if (!owned_source)
		return NB_ERR_NOMEM;
	free(dt->source);
	dt->source = owned_source;
	source = owned_source;
	rc = nb_file_write_all_atomic(
		dt->source_path, source, strlen(source),
		&(nb_file_write_options){.new_file_mode = 0600, .sync_dir = true}, NULL);
	if (rc < 0)
		return rc;
	rc = ((committed_metadata_path_status = nb_path_concat(
		       &committed_metadata_path_buffer, dt->dir, DYN_TOOL_META_FILE)) != NB_OK
		      ? committed_metadata_path_status
	      : (committed_metadata_path_status =
			 nb_str_copy_to(nb_buf_str(&committed_metadata_path_buffer), meta_path,
					sizeof(meta_path))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : committed_metadata_path_status);
	if (rc < 0)
		return rc;
	meta = cJSON_CreateObject();
	if (!meta)
		return NB_ERR_NOMEM;
	if (!cJSON_AddStringToObject(meta, "source", source)) {
		cJSON_Delete(meta);
		return NB_ERR_NOMEM;
	}
	if (!cJSON_AddStringToObject(meta, "name", dt->name) ||
	    !cJSON_AddStringToObject(meta, "description", dt->description ? dt->description : "") ||
	    !cJSON_AddStringToObject(meta, "input_schema", dt->input_schema) ||
	    !cJSON_AddStringToObject(meta, "output_schema", dt->output_schema)) {
		cJSON_Delete(meta);
		return NB_ERR_NOMEM;
	}
	meta_json = cJSON_PrintUnformatted(meta);
	cJSON_Delete(meta);
	if (!meta_json)
		return NB_ERR_NOMEM;
	rc = nb_file_write_all_atomic(
		meta_path, meta_json, strlen(meta_json),
		&(nb_file_write_options){.new_file_mode = 0600, .sync_dir = true}, NULL);
	free(meta_json);
	return rc;
}

int dynamic_tool_store_tool_file_paths(const char *dir, char *meta_path, size_t meta_size,
				       char *source_path, size_t source_size)
{
	nb_buf metadata_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int metadata_path_status = NB_OK;
	nb_buf source_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int source_path_status = NB_OK;

	int rc;

	if (!dir)
		return NB_ERR_INVAL;
	rc = ((metadata_path_status =
		       nb_path_concat(&metadata_path_buffer, dir, DYN_TOOL_META_FILE)) != NB_OK
		      ? metadata_path_status
	      : (metadata_path_status = nb_str_copy_to(nb_buf_str(&metadata_path_buffer), meta_path,
						       meta_size)) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : metadata_path_status);
	if (rc < 0)
		return rc;
	return ((source_path_status =
			 nb_path_concat(&source_path_buffer, dir, DYN_TOOL_SOURCE_FILE)) != NB_OK
			? source_path_status
		: (source_path_status = nb_str_copy_to(nb_buf_str(&source_path_buffer), source_path,
						       source_size)) == NB_ERR_RANGE
			? NB_ERR_NAME_TOO_LONG
			: source_path_status);
}

int dynamic_tool_store_history_dir_path(const char *tool_dir, char *path, size_t path_size)
{
	nb_buf history_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int history_path_status = NB_OK;

	return ((history_path_status = nb_path_concat(&history_path_buffer, tool_dir,
						      DYN_TOOL_HISTORY_DIR)) != NB_OK
			? history_path_status
		: (history_path_status = nb_str_copy_to(nb_buf_str(&history_path_buffer), path,
							path_size)) == NB_ERR_RANGE
			? NB_ERR_NAME_TOO_LONG
			: history_path_status);
}

int dynamic_tool_store_checkpoint_dir_path(const char *tool_dir, const char *checkpoint_id,
					   char *path, size_t path_size)
{
	nb_buf checkpoint_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int checkpoint_path_status = NB_OK;

	char history[PATH_MAX];
	int rc;

	rc = dynamic_tool_store_history_dir_path(tool_dir, history, sizeof(history));
	if (rc < 0)
		return rc;
	return ((checkpoint_path_status =
			 nb_path_concat(&checkpoint_path_buffer, history, checkpoint_id)) != NB_OK
			? checkpoint_path_status
		: (checkpoint_path_status = nb_str_copy_to(nb_buf_str(&checkpoint_path_buffer),
							   path, path_size)) == NB_ERR_RANGE
			? NB_ERR_NAME_TOO_LONG
			: checkpoint_path_status);
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
		return NB_ERR_INVAL;
	for (const char *p = id; *p; p++) {
		if (*p < '0' || *p > '9')
			return NB_ERR_INVAL;
		value = value * 10 + (*p - '0');
	}
	return value;
}

int dynamic_tool_store_latest_checkpoint_id(const char *tool_dir, char *id, size_t id_size)
{
	int directory_scan_status = NB_OK;

	char history[PATH_MAX];
	nb_dir_listing dirs NB_CLEANUP(nb_dir_listing_destroy) = {0};
	int count = 0;
	int max_id = -1;
	int rc;

	if (!id || id_size == 0)
		return NB_ERR_INVAL;
	id[0] = '\0';
	rc = dynamic_tool_store_history_dir_path(tool_dir, history, sizeof(history));
	if (rc < 0)
		return rc;
	if (!nb_file_exists(history))
		return NB_ERR_NOT_FOUND;
	if (((directory_scan_status = nb_dir_scan(
		      history, NB_DIR_SCAN_DIRS | NB_DIR_SCAN_HIDDEN | NB_DIR_SCAN_FOLLOW_LINKS,
		      &dirs, NULL)) != NB_OK
		     ? directory_scan_status
	     : dirs.names.len > INT_MAX ? NB_ERR_RANGE
					: (count = (int)dirs.names.len, NB_OK)) != 0)
		return NB_ERR_NOT_FOUND;
	for (int i = 0; i < count; i++) {
		int value = dynamic_tool_store_checkpoint_id_value(
			nb_dir_listing_name(&dirs, (size_t)(i)));
		if (value > max_id)
			max_id = value;
	}
	nb_dir_listing_destroy(&dirs);
	if (max_id < 0)
		return NB_ERR_NOT_FOUND;
	if (snprintf(id, id_size, "%04d", max_id) >= (int)id_size)
		return NB_ERR_NAME_TOO_LONG;
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
	else if (rc != NB_ERR_NOT_FOUND)
		return rc;
	value++;
	if (snprintf(id, id_size, "%04d", value) >= (int)id_size)
		return NB_ERR_NAME_TOO_LONG;
	return 0;
}

int dynamic_tool_store_write_checkpoint_meta(const char *checkpoint_dir,
					     const struct tool_checkpoint *cp)
{
	nb_buf metadata_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int metadata_path_status = NB_OK;

	char path[PATH_MAX];
	cJSON *root;
	char *json;
	int rc;

	rc = ((metadata_path_status = nb_path_concat(&metadata_path_buffer, checkpoint_dir,
						     DYN_TOOL_CHECKPOINT_META_FILE)) != NB_OK
		      ? metadata_path_status
	      : (metadata_path_status = nb_str_copy_to(nb_buf_str(&metadata_path_buffer), path,
						       sizeof(path))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : metadata_path_status);
	if (rc < 0)
		return rc;
	root = cJSON_CreateObject();
	if (!root)
		return NB_ERR_NOMEM;
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
		return NB_ERR_NOMEM;
	rc = nb_file_write_all_atomic(
		path, json, strlen(json),
		&(nb_file_write_options){.new_file_mode = 0600, .sync_dir = true}, NULL);
	free(json);
	return rc;
}

int dynamic_tool_store_read_checkpoint_meta(const char *checkpoint_dir, struct tool_checkpoint *cp)
{
	nb_buf metadata_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int metadata_path_status = NB_OK;
	nb_buf metadata_contents_buffer NB_CLEANUP(nb_buf_destroy) = {0};

	char path[PATH_MAX];
	cJSON *root;
	cJSON *item;
	int rc;

	if (!cp)
		return NB_ERR_INVAL;
	memset(cp, 0, sizeof(*cp));
	rc = ((metadata_path_status = nb_path_concat(&metadata_path_buffer, checkpoint_dir,
						     DYN_TOOL_CHECKPOINT_META_FILE)) != NB_OK
		      ? metadata_path_status
	      : (metadata_path_status = nb_str_copy_to(nb_buf_str(&metadata_path_buffer), path,
						       sizeof(path))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : metadata_path_status);
	if (rc < 0)
		return rc;
	rc = nb_file_read_all(path, &metadata_contents_buffer, 0, NULL);
	if (rc != NB_OK)
		return rc;
	root = cJSON_Parse(nb_buf_cstr(&metadata_contents_buffer));
	nb_buf_destroy(&metadata_contents_buffer);
	if (!root)
		return NB_ERR_INVAL;
	item = cJSON_GetObjectItem(root, "id");
	if (cJSON_IsString(item) && item->valuestring)
		(void)nb_str_copy_to(nb_str_from_parts(item->valuestring,
						       nb_utf8_clamp_bytes(item->valuestring,
									   sizeof(cp->id) - 1)),
				     cp->id, (sizeof(cp->id) - 1) + 1);
	item = cJSON_GetObjectItem(root, "name");
	if (cJSON_IsString(item) && item->valuestring)
		(void)nb_str_copy_to(nb_str_from_parts(item->valuestring,
						       nb_utf8_clamp_bytes(item->valuestring,
									   sizeof(cp->name) - 1)),
				     cp->name, (sizeof(cp->name) - 1) + 1);
	item = cJSON_GetObjectItem(root, "before_state");
	if (cJSON_IsString(item) && item->valuestring)
		(void)nb_str_copy_to(
			nb_str_from_parts(item->valuestring,
					  nb_utf8_clamp_bytes(item->valuestring,
							      sizeof(cp->before_state) - 1)),
			cp->before_state, (sizeof(cp->before_state) - 1) + 1);
	item = cJSON_GetObjectItem(root, "origin");
	if (cJSON_IsString(item) && item->valuestring)
		(void)nb_str_copy_to(nb_str_from_parts(item->valuestring,
						       nb_utf8_clamp_bytes(item->valuestring,
									   sizeof(cp->origin) - 1)),
				     cp->origin, (sizeof(cp->origin) - 1) + 1);
	item = cJSON_GetObjectItem(root, "old_hash");
	if (cJSON_IsString(item) && item->valuestring)
		(void)nb_str_copy_to(
			nb_str_from_parts(
				item->valuestring,
				nb_utf8_clamp_bytes(item->valuestring, sizeof(cp->old_hash) - 1)),
			cp->old_hash, (sizeof(cp->old_hash) - 1) + 1);
	item = cJSON_GetObjectItem(root, "new_hash");
	if (cJSON_IsString(item) && item->valuestring)
		(void)nb_str_copy_to(
			nb_str_from_parts(
				item->valuestring,
				nb_utf8_clamp_bytes(item->valuestring, sizeof(cp->new_hash) - 1)),
			cp->new_hash, (sizeof(cp->new_hash) - 1) + 1);
	item = cJSON_GetObjectItem(root, "created_at");
	if (cJSON_IsNumber(item))
		cp->created_at = (long)item->valuedouble;
	cJSON_Delete(root);
	if (!cp->id[0] || !cp->name[0] || !cp->before_state[0])
		return NB_ERR_INVAL;
	return 0;
}

int dynamic_tool_store_remove_checkpoint_ids_from(const char *tool_dir, int min_id)
{
	int directory_scan_status = NB_OK;

	nb_buf checkpoint_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int checkpoint_path_status = NB_OK;

	char history[PATH_MAX];
	nb_dir_listing dirs NB_CLEANUP(nb_dir_listing_destroy) = {0};
	int count = 0;
	int rc;

	rc = dynamic_tool_store_history_dir_path(tool_dir, history, sizeof(history));
	if (rc < 0)
		return rc;
	if (!nb_file_exists(history))
		return 0;
	if (((directory_scan_status = nb_dir_scan(
		      history, NB_DIR_SCAN_DIRS | NB_DIR_SCAN_HIDDEN | NB_DIR_SCAN_FOLLOW_LINKS,
		      &dirs, NULL)) != NB_OK
		     ? directory_scan_status
	     : dirs.names.len > INT_MAX ? NB_ERR_RANGE
					: (count = (int)dirs.names.len, NB_OK)) != 0)
		return 0;
	for (int i = 0; i < count; i++) {
		int value = dynamic_tool_store_checkpoint_id_value(
			nb_dir_listing_name(&dirs, (size_t)(i)));
		char path[PATH_MAX];

		if (value < min_id)
			continue;
		if (((checkpoint_path_status =
			      nb_path_concat(&checkpoint_path_buffer, history,
					     nb_dir_listing_name(&dirs, (size_t)(i)))) != NB_OK
			     ? checkpoint_path_status
		     : (checkpoint_path_status = nb_str_copy_to(nb_buf_str(&checkpoint_path_buffer),
								path, sizeof(path))) == NB_ERR_RANGE
			     ? NB_ERR_NAME_TOO_LONG
			     : checkpoint_path_status) != 0)
			continue;
		(void)dynamic_tool_store_remove_tree(path);
	}
	nb_dir_listing_destroy(&dirs);
	return 0;
}

int dynamic_tool_store_prune_checkpoint_history(const char *tool_dir)
{
	int directory_scan_status = NB_OK;

	nb_buf checkpoint_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int checkpoint_path_status = NB_OK;

	char history[PATH_MAX];
	nb_dir_listing dirs NB_CLEANUP(nb_dir_listing_destroy) = {0};
	int count = 0;
	int rc;

	rc = dynamic_tool_store_history_dir_path(tool_dir, history, sizeof(history));
	if (rc < 0)
		return rc;
	if (!nb_file_exists(history))
		return 0;
	if (((directory_scan_status = nb_dir_scan(
		      history, NB_DIR_SCAN_DIRS | NB_DIR_SCAN_HIDDEN | NB_DIR_SCAN_FOLLOW_LINKS,
		      &dirs, NULL)) != NB_OK
		     ? directory_scan_status
	     : dirs.names.len > INT_MAX ? NB_ERR_RANGE
					: (count = (int)dirs.names.len, NB_OK)) != 0)
		return 0;
	while (count > DYN_TOOL_HISTORY_LIMIT) {
		int min_idx = -1;
		int min_id = INT_MAX;

		for (int i = 0; i < count; i++) {
			int value = dynamic_tool_store_checkpoint_id_value(
				nb_dir_listing_name(&dirs, (size_t)(i)));
			if (value >= 0 && value < min_id) {
				min_id = value;
				min_idx = i;
			}
		}
		if (min_idx < 0)
			break;
		{
			char path[PATH_MAX];
			if (((checkpoint_path_status = nb_path_concat(
				      &checkpoint_path_buffer, history,
				      nb_dir_listing_name(&dirs, (size_t)(min_idx)))) != NB_OK
				     ? checkpoint_path_status
			     : (checkpoint_path_status =
					nb_str_copy_to(nb_buf_str(&checkpoint_path_buffer), path,
						       sizeof(path))) == NB_ERR_RANGE
				     ? NB_ERR_NAME_TOO_LONG
				     : checkpoint_path_status) == 0)
				(void)dynamic_tool_store_remove_tree(path);
		}
		nb_buf_destroy(nb_vec_at(&dirs.names, (size_t)min_idx));
		nb_vec_remove(&dirs.names, (size_t)min_idx);
		count--;
	}
	nb_dir_listing_destroy(&dirs);
	return 0;
}

int dynamic_tool_store_create_tool_checkpoint(const char *tool_dir,
					      const struct dynamic_tool *old_dt, const char *name,
					      struct tool_checkpoint *cp, char *checkpoint_dir,
					      size_t checkpoint_dir_size)
{
	nb_buf source_contents_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	char *source_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

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
		return NB_ERR_INVAL;
	memset(cp, 0, sizeof(*cp));
	rc = nb_file_mkdir(tool_dir, true, NULL);
	if (rc < 0)
		return rc;
	rc = dynamic_tool_store_history_dir_path(tool_dir, history, sizeof(history));
	if (rc == 0)
		rc = nb_file_mkdir(history, true, NULL);
	if (rc == 0)
		rc = next_checkpoint_id(tool_dir, id, sizeof(id));
	if (rc == 0)
		rc = dynamic_tool_store_checkpoint_dir_path(tool_dir, id, checkpoint_dir,
							    checkpoint_dir_size);
	if (rc == 0)
		rc = nb_file_mkdir(checkpoint_dir, true, NULL);
	if (rc < 0)
		return rc;
	(void)nb_str_copy_to(nb_str_from_parts(id, nb_utf8_clamp_bytes(id, sizeof(cp->id) - 1)),
			     cp->id, (sizeof(cp->id) - 1) + 1);
	(void)nb_str_copy_to(
		nb_str_from_parts(name, nb_utf8_clamp_bytes(name, sizeof(cp->name) - 1)), cp->name,
		(sizeof(cp->name) - 1) + 1);
	(void)nb_str_copy_to(
		nb_str_from_parts("absent",
				  nb_utf8_clamp_bytes("absent", sizeof(cp->before_state) - 1)),
		cp->before_state, (sizeof(cp->before_state) - 1) + 1);
	(void)nb_str_copy_to(
		nb_str_from_parts("none", nb_utf8_clamp_bytes("none", sizeof(cp->origin) - 1)),
		cp->origin, (sizeof(cp->origin) - 1) + 1);
	(void)nb_str_copy_to(
		nb_str_from_parts("absent",
				  nb_utf8_clamp_bytes("absent", sizeof(cp->old_hash) - 1)),
		cp->old_hash, (sizeof(cp->old_hash) - 1) + 1);
	cp->created_at = (long)time(NULL);
	if (old_dt) {
		(void)nb_str_copy_to(
			nb_str_from_parts(
				"present",
				nb_utf8_clamp_bytes("present", sizeof(cp->before_state) - 1)),
			cp->before_state, (sizeof(cp->before_state) - 1) + 1);
		(void)nb_str_copy_to(
			nb_str_from_parts(origin_name(old_dt->origin),
					  nb_utf8_clamp_bytes(origin_name(old_dt->origin),
							      sizeof(cp->origin) - 1)),
			cp->origin, (sizeof(cp->origin) - 1) + 1);
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
			source = (nb_file_read_all(cp_source, &source_contents_buffer, 0, NULL) ==
							  NB_OK &&
						  (source_contents_copy = (char *)malloc(
							   nb_buf_len(&source_contents_buffer) +
							   1)) != NULL
					  ? (memcpy(source_contents_copy,
						    nb_buf_cstr(&source_contents_buffer),
						    nb_buf_len(&source_contents_buffer) + 1),
					     *(&source_len) = nb_buf_len(&source_contents_buffer),
					     nb_charp_steal(&source_contents_copy))
					  : NULL);
			if (!source)
				rc = NB_ERR_NOT_FOUND;
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
		return NB_ERR_INVAL;
	if (strcmp(cp->before_state, "absent") == 0) {
		if (nb_file_exists(tool_dir))
			return dynamic_tool_store_remove_tree(tool_dir);
		return 0;
	}
	if (strcmp(cp->before_state, "present") != 0)
		return NB_ERR_INVAL;
	rc = nb_file_mkdir(tool_dir, true, NULL);
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

	if (!path || !*path)
		return NB_ERR_INVAL;
	/* Preserve the public missing-root error; native removal itself is idempotent. */
	if (lstat(path, &st) != 0)
		return nb_status_from_errno(errno, NULL);
	return nb_file_remove_all(path, 0, NULL);
}

char *dynamic_tool_store_optional_text_create(const char *path)
{
	nb_buf file_contents_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

	if (!path || !nb_file_exists(path))
		return strdup("");
	return (nb_file_read_all(path, &file_contents_buffer, 0, NULL) == NB_OK &&
				(file_contents_copy = (char *)malloc(
					 nb_buf_len(&file_contents_buffer) + 1)) != NULL
			? (memcpy(file_contents_copy, nb_buf_cstr(&file_contents_buffer),
				  nb_buf_len(&file_contents_buffer) + 1),
			   nb_charp_steal(&file_contents_copy))
			: NULL);
}

int dynamic_tool_store_append_whole_file_diff(nb_buf *buf, const char *label, const char *old_text,
					      const char *new_text)
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
	rc = (nb_buf_printf(buf, "--- a/%s\n+++ b/%s\n@@ -1,%d +1,%d @@\n", label, label, old_lines,
			    new_lines)
		      ? 0
		      : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	for (const char *p = old_ptr; *p;) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) + 1 : strlen(p);

		rc = (nb_buf_append_char(buf, '-') ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append(buf, p, len) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
		if (!eol)
			rc = (nb_buf_append_char(buf, '\n') ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
		p += len;
	}
	for (const char *p = new_ptr; *p;) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) + 1 : strlen(p);

		rc = (nb_buf_append_char(buf, '+') ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append(buf, p, len) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
		if (!eol)
			rc = (nb_buf_append_char(buf, '\n') ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
		p += len;
	}
	return 0;
}
