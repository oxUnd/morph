#include "file_list.h"
#include "agent/tool_context.h"
#include "agent/tool_runtime.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "errors.h"
#include "nb/utf8.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FILE_LIST_DEFAULT_LIMIT 1000
#define FILE_LIST_MAX_LIMIT 10000
#define FILE_LIST_DEFAULT_BYTES (50 * 1024)
#define FILE_LIST_MAX_BYTES (1024 * 1024)
#define FILE_LIST_MIN_BYTES 2048

struct list_entry {
	char *name;
	const char *type;
};

struct list_options {
	const char *path;
	size_t limit;
	size_t max_bytes;
	struct list_entry after;
	int has_after;
};

static int entry_compare(const void *left, const void *right)
{
	const struct list_entry *a = left;
	const struct list_entry *b = right;
	int a_dir = strcmp(a->type, "dir") == 0;
	int b_dir = strcmp(b->type, "dir") == 0;

	if (a_dir != b_dir)
		return b_dir - a_dir;
	return strcmp(a->name, b->name);
}

static int entry_name_validate(const char *name)
{
	size_t len = strlen(name);

	if (len == 0)
		return NB_ERR_INVAL;
	for (size_t pos = 0; pos < len;) {
		unsigned cp;
		size_t count;

		if (!nb_utf8_read(name + pos, len - pos, &cp, &count))
			return NB_ERR_ENCODING;
		pos += count;
	}
	return 0;
}

static int list_integer(cJSON *root, const char *key, size_t min,
			size_t max, size_t *value)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

	if (!item)
		return 0;
	if (!cJSON_IsNumber(item) || !(item->valuedouble >= (double)min) ||
	    !(item->valuedouble <= (double)max))
		return NB_ERR_INVAL;
	size_t number = (size_t)item->valuedouble;
	if ((double)number != item->valuedouble)
		return NB_ERR_INVAL;
	*value = number;
	return 0;
}

static int list_options_parse(cJSON *root, struct list_options *opts)
{
	cJSON *path = cJSON_GetObjectItemCaseSensitive(root, "dir_path");
	cJSON *after = cJSON_GetObjectItemCaseSensitive(root, "after");

	if (!cJSON_IsObject(root) || !cJSON_IsString(path) ||
	    entry_name_validate(path->valuestring) < 0)
		return NB_ERR_INVAL;
	opts->path = path->valuestring;
	opts->limit = FILE_LIST_DEFAULT_LIMIT;
	opts->max_bytes = FILE_LIST_DEFAULT_BYTES;
	if (list_integer(root, "limit", 1, FILE_LIST_MAX_LIMIT, &opts->limit) < 0 ||
	    list_integer(root, "max_bytes", FILE_LIST_MIN_BYTES,
			 FILE_LIST_MAX_BYTES, &opts->max_bytes) < 0)
		return NB_ERR_INVAL;
	if (!after)
		return 0;
	cJSON *name = cJSON_GetObjectItemCaseSensitive(after, "name");
	cJSON *type = cJSON_GetObjectItemCaseSensitive(after, "type");
	if (!cJSON_IsObject(after) || !cJSON_IsString(name) || !cJSON_IsString(type))
		return NB_ERR_INVAL;
	const char *kind = type->valuestring;
	if (strcmp(kind, "dir") && strcmp(kind, "file") && strcmp(kind, "symlink") &&
	    strcmp(kind, "fifo") && strcmp(kind, "socket") && strcmp(kind, "char") &&
	    strcmp(kind, "block") && strcmp(kind, "unknown"))
		return NB_ERR_INVAL;
	if (entry_name_validate(name->valuestring) < 0)
		return NB_ERR_INVAL;
	opts->after.name = name->valuestring;
	opts->after.type = kind;
	opts->has_after = 1;
	return 0;
}

static const char *entry_type(DIR *directory, const struct dirent *entry, int *error)
{
#ifdef DT_UNKNOWN
	switch (entry->d_type) {
	case DT_DIR: return "dir";
	case DT_REG: return "file";
	case DT_LNK: return "symlink";
	case DT_FIFO: return "fifo";
	case DT_SOCK: return "socket";
	case DT_CHR: return "char";
	case DT_BLK: return "block";
	default: break;
	}
#endif
	struct stat st;
	if (fstatat(dirfd(directory), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
		*error = (nb_status_from_errno(errno, NULL));
		return NULL;
	}
	if (S_ISDIR(st.st_mode)) return "dir";
	if (S_ISREG(st.st_mode)) return "file";
	if (S_ISLNK(st.st_mode)) return "symlink";
	if (S_ISFIFO(st.st_mode)) return "fifo";
	if (S_ISSOCK(st.st_mode)) return "socket";
	if (S_ISCHR(st.st_mode)) return "char";
	if (S_ISBLK(st.st_mode)) return "block";
	return "unknown";
}

/* Keep only the first page-sized set in a max-heap: O(n log limit), O(limit) memory. */
static int list_keep_entry(nb_vec *items, const struct list_entry *entry, size_t capacity)
{
	size_t index;
	struct list_entry *values = ((void *)items->data);

	if (items->len == capacity && entry_compare(entry, &values[0]) >= 0)
		return 0;
	char *name = strdup(entry->name);
	if (!name)
		return NB_ERR_NOMEM;

	if (items->len < capacity) {
		struct list_entry *slot = nb_vec_push(items);
		if (!slot) {
			free(name);
			return NB_ERR_NOMEM;
		}
		*slot = *entry;
		slot->name = name;
		index = items->len - 1;
		values = ((void *)items->data);
		while (index > 0) {
			size_t parent = (index - 1) / 2;
			if (entry_compare(&values[parent], &values[index]) >= 0)
				break;
			struct list_entry swap = values[parent];
			values[parent] = values[index];
			values[index] = swap;
			index = parent;
		}
		return 0;
	}
	free(values[0].name);
	values[0] = *entry;
	values[0].name = name;
	index = 0;
	while (index * 2 + 1 < items->len) {
		size_t child = index * 2 + 1;
		if (child + 1 < items->len && entry_compare(&values[child + 1], &values[child]) > 0)
			child++;
		if (entry_compare(&values[index], &values[child]) >= 0)
			break;
		struct list_entry swap = values[index];
		values[index] = values[child];
		values[child] = swap;
		index = child;
	}
	return 0;
}

static int list_scan(DIR *directory, const struct list_options *opts,
		     const struct http_control *control, nb_vec *items, size_t *total,
		     size_t *eligible)
{
	for (;;) {
		if ((*total % 64) == 0) {
			int rc = http_wait_cancelable(control, 0);
			if (rc < 0)
				return rc;
		}
		errno = 0;
		struct dirent *entry = readdir(directory);
		if (!entry) {
			if (errno)
				return nb_status_from_errno(errno, NULL);
			return 0;
		}
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		(*total)++;
		struct list_entry candidate = {0};
		int rc = entry_name_validate(entry->d_name);
		if (rc < 0)
			return rc;
		candidate.name = entry->d_name;
		candidate.type = entry_type(directory, entry, &rc);
		if (!candidate.type)
			return rc;
		if (opts->has_after && entry_compare(&candidate, &opts->after) <= 0)
			continue;
		(*eligible)++;
		rc = list_keep_entry(items, &candidate, opts->limit);
		if (rc < 0)
			return rc;
	}
}

static cJSON *list_entry_json(const struct list_entry *entry)
{
	cJSON *item = cJSON_CreateObject();

	if (!item)
		return NULL;
	if (!cJSON_AddStringToObject(item, "name", entry->name) ||
	    !cJSON_AddStringToObject(item, "type", entry->type)) {
		cJSON_Delete(item);
		return NULL;
	}
	return item;
}

static int list_result(const struct list_options *opts, nb_vec *items, size_t total,
		       size_t eligible, struct tool_result *result)
{
	cJSON *out NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *entries = out ? cJSON_AddArrayToObject(out, "entries") : NULL;
	size_t returned = 0;
	size_t bytes = 2;

	if (!entries)
		return NB_ERR_NOMEM;
	qsort(((void *)items->data), items->len, items->elem_size, entry_compare);
	for (size_t i = 0; i < items->len; i++) {
		if ((i % 64) == 0) {
			const struct http_control *control = result->runtime ?
				&result->runtime->http : NULL;
			int rc = http_wait_cancelable(control, 0);

			if (rc < 0)
				return rc;
		}
		const struct list_entry *entry = nb_vec_at(items, i);
		cJSON *item NB_CLEANUP(cJSON_DeleteOwner) = list_entry_json(entry);
		char *json NB_CLEANUP(cJSON_FreeTextOwner) =
			item ? cJSON_PrintUnformatted(item) : NULL;

		if (!json)
			return NB_ERR_NOMEM;
		size_t length = strlen(json) + (returned != 0);
		if (bytes + length > opts->max_bytes) {
			if (returned == 0)
				return NB_ERR_TOO_BIG;
			break;
		}
		if ((cJSON_AddItemToArray(entries, item) ? (item = NULL, NB_OK) : NB_ERR_NOMEM) !=
		    0)
			return NB_ERR_NOMEM;
		bytes += length;
		returned++;
	}
	int more = returned < eligible;
	cJSON *next NB_CLEANUP(cJSON_DeleteOwner) =
		more && returned ? list_entry_json(nb_vec_at(items, returned - 1))
				 : cJSON_CreateNull();

	if (!next || (cJSON_AddItemToObject(out, "next_after", next) ? (next = NULL, NB_OK)
								     : NB_ERR_NOMEM) != 0)
		return NB_ERR_NOMEM;
	if (!cJSON_AddStringToObject(out, "dir_path", opts->path) ||
	    !cJSON_AddNumberToObject(out, "total_entries", (double)total) ||
	    !cJSON_AddNumberToObject(out, "returned_entries", (double)returned) ||
	    !cJSON_AddBoolToObject(out, "truncated", more) ||
	    !cJSON_AddStringToObject(out, "truncation_reason",
				     more ? (returned < items->len ? "max_bytes" : "limit") : ""))
		return NB_ERR_NOMEM;
	if (more && !cJSON_AddStringToObject(out, "note",
		"Continue with after=next_after and the same dir_path. "
		"Directory contents may change between pages."))
		return NB_ERR_NOMEM;
	return tool_result_success_take(result, &out);
}

static void list_entries_cleanup(nb_vec *items)
{
	struct list_entry *item;

	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(items) && (item = (struct list_entry *)nb_vec_at(items, _i_)); _i_++)
		free(item->name);
	nb_vec_cleanup(items);
}

static void list_directory_cleanup(DIR **directory)
{
	if (*directory)
		(void)closedir(*directory);
	*directory = NULL;
}

static int list_execute(struct tool_context *tctx, const struct list_options *opts,
			struct tool_result *result)
{
	nb_vec items NB_CLEANUP(list_entries_cleanup) = {0};
	DIR *directory NB_CLEANUP(list_directory_cleanup) = NULL;
	char resolved[PATH_MAX];
	size_t total = 0;
	size_t eligible = 0;
	const struct http_control *control = result->runtime ? &result->runtime->http : NULL;
	int rc = http_wait_cancelable(control, 0);

	if (rc < 0)
		return rc;
	if (tctx) {
		rc = tool_context_authorize_path(tctx, TOOL_PATH_LIST, opts->path,
					 resolved, sizeof(resolved));
		if (rc < 0)
			return rc;
	} else if (strlen(opts->path) >= sizeof(resolved)) {
		return NB_ERR_NAME_TOO_LONG;
	} else {
		memcpy(resolved, opts->path, strlen(opts->path) + 1);
	}
	directory = opendir(resolved);
	if (!directory)
		return nb_status_from_errno(errno, NULL);
	struct list_entry sample;
	rc = (nb_vec_init(&items, sizeof(sample)),
	      nb_vec_reserve(&items, opts->limit) ? 0 : NB_ERR_NOMEM);
	if (rc < 0)
		return rc;
	rc = list_scan(directory, opts, control, &items, &total, &eligible);
	if (rc == 0)
		rc = http_wait_cancelable(control, 0);
	if (closedir(directory) < 0 && rc == 0)
		rc = nb_status_from_errno(errno, NULL);
	directory = NULL;
	if (rc == 0)
		rc = list_result(opts, &items, total, eligible, result);
	return rc;
}

static int file_list_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct tool_context *tctx = user_data;
	struct list_options opts = {0};
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!result)
		return NB_ERR_INVAL;
	root = args_json ? cJSON_Parse(args_json) : NULL;
	int rc = list_options_parse(root, &opts);
	if (rc < 0) {
		(void)tool_result_error_copy(
			result, "invalid_arguments",
			"Provide dir_path, limit (1..10000), max_bytes (2048..1048576), "
			"and optional after={name,type} from next_after.");
		return rc;
	}
	rc = list_execute(tctx, &opts, result);
	if (rc < 0) {
		const char *message =
			rc == NB_ERR_NOT_PERMITTED
				? "list path outside workspace: permission denied"
			: rc == NB_ERR_TOO_BIG
				? "Directory entry exceeds max_bytes; increase max_bytes."
			: rc == NB_ERR_ENCODING
				? "Directory contains a name that is not valid UTF-8."
				: nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc);
		(void)tool_result_error_copy(result, "file_list_failed", message);
	}
	return rc;
}

int file_list_init(struct tool_registry *reg, struct tool_context *tctx)
{
	if (!reg)
		return NB_ERR_INVAL;
	return tool_register_borrow(
		reg,
		&(struct tool_spec){
			.origin = TOOL_ORIGIN_BUILTIN,
			.name = "file_list",
			.description = "List one directory, including hidden and ignored entries. "
				       "Directories first, then other entries, sorted by name. "
				       "Symlinks are not followed. "
				       "limit defaults to 1000 (1..10000); max_bytes bounds the "
				       "entries JSON array "
				       "and defaults to 51200 (2048..1048576). If truncated, pass "
				       "next_after as after "
				       "with the same dir_path to continue. Pagination is best "
				       "effort if contents change.",
			.input_schema =
				"{\"type\":\"object\",\"properties\":{"
				"\"dir_path\":{\"type\":\"string\"},"
				"\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":10000},"
				"\"max_bytes\":{\"type\":\"integer\",\"minimum\":2048,\"maximum\":"
				"1048576},"
				"\"after\":{\"type\":\"object\",\"properties\":{"
				"\"name\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"}},"
				"\"required\":[\"name\",\"type\"]}},\"required\":[\"dir_path\"]}",
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.exec = file_list_exec,
			.user_data = tctx,
		});
}
