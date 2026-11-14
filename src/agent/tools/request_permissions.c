#include "request_permissions.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "errors.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/vec.h"
#include "cJSON.h"
#include "cJSON.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct directory_request {
	char *path;
	unsigned int access;
};

static const struct {
	const char *name;
	unsigned int access;
} permissions[] = {
	{"read", TOOL_PATH_ACCESS_READ},
	{"write", TOOL_PATH_ACCESS_WRITE},
	{"delete", TOOL_PATH_ACCESS_DELETE},
};

static int collect_requests(cJSON *file_system, nb_vec *requests, const char **failed_path)
{
	for (size_t i = 0; i < sizeof(permissions) / sizeof(permissions[0]); i++) {
		cJSON *paths = cJSON_GetObjectItemCaseSensitive(file_system, permissions[i].name);
		cJSON *path;

		if (!paths)
			continue;
		if (!cJSON_IsArray(paths))
			return NB_ERR_INVAL;
		cJSON_ArrayForEach(path, paths) {
			char canonical[PATH_MAX];

			if (!cJSON_IsString(path))
				return NB_ERR_INVAL;
			*failed_path = path->valuestring;
			/* Merge before validating a path created by the same write request. */
			int rc = tool_context_resolve_directory_access(path->valuestring,
				TOOL_PATH_ACCESS_ALL, canonical, sizeof(canonical));

			if (rc != 0)
				return rc;
			struct directory_request *entry;
			int found = 0;

			for (size_t _i_ = 0;
			     _i_ < nb_vec_len(requests) &&
			     (entry = (struct directory_request *)nb_vec_at(requests, _i_));
			     _i_++) {
				if (strcmp(entry->path, canonical) == 0) {
					entry->access |= permissions[i].access;
					found = 1;
					break;
				}
			}
			if (found)
				continue;
			char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(canonical);

			entry = copy ? nb_vec_push(requests) : NULL;
			if (!entry)
				return NB_ERR_NOMEM;
			entry->path = copy;
			copy = NULL;
			entry->access = permissions[i].access;
		}
	}
	struct directory_request *entry;

	/* Validate every directory before asking the user about any of them. */
	for (size_t _i_ = 0; _i_ < nb_vec_len(requests) &&
			     (entry = (struct directory_request *)nb_vec_at(requests, _i_));
	     _i_++) {
		char canonical[PATH_MAX];

		*failed_path = entry->path;
		int rc = tool_context_resolve_directory_access(entry->path, entry->access,
			canonical, sizeof(canonical));

		if (rc != 0)
			return rc;
	}
	return 0;
}

static void directory_requests_cleanup(nb_vec *requests)
{
	if (!requests)
		return;
	for (size_t i = 0; i < requests->len; i++) {
		struct directory_request *entry = nb_vec_at(requests, i);
		free(entry->path);
		entry->path = NULL;
	}
	nb_vec_cleanup(requests);
}

static int request_parent_first(const void *left, const void *right)
{
	const struct directory_request *a = left;
	const struct directory_request *b = right;
	size_t a_length = strlen(a->path);
	size_t b_length = strlen(b->path);

	if (a_length != b_length)
		return a_length < b_length ? -1 : 1;
	return strcmp(a->path, b->path);
}

static int permission_error(struct tool_result *result, int error,
	const char *path, int validation)
{
	const char *code = validation			   ? "invalid_arguments"
			   : error == NB_ERR_PERMISSION	   ? "permission_denied"
			   : error == NB_ERR_NOT_PERMITTED ? "approval_unavailable"
							   : "permission_request_failed";

	return tool_result_error_format(
		result, code, "Directory permission request failed%s%s: %s", path ? " for " : "",
		path ? path : "", nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, error));
}

static int request_permissions_run(const char *args_json,
	struct tool_result *result, void *user_data)
{
	struct tool_context *tctx = user_data;
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) =
		args_json ? cJSON_ParseWithOpts(args_json, NULL, 1) : NULL;
	cJSON *data NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	nb_vec requests NB_CLEANUP(directory_requests_cleanup) = {0};
	const char *failed_path = NULL;
	struct directory_request *entry;
	int rc;

	if (!tctx || !cJSON_IsObject(root)) {
		return tool_result_error_copy(result, "invalid_arguments",
					      "Expected a permission request.");
	}
	const char *command = cJSON_GetStringValue(cJSON_GetObjectItem(root, "command"));
	const char *scope = cJSON_GetStringValue(cJSON_GetObjectItem(root, "scope"));
	const char *reason = cJSON_GetStringValue(cJSON_GetObjectItem(root, "justification"));
	cJSON *permission = cJSON_GetObjectItem(root, "permissions");
	cJSON *file_system = cJSON_GetObjectItem(permission, "file_system");

	if (!command || !*command)
		command = "Access the requested directories";
	if (!scope)
		scope = "turn";
	if ((strcmp(scope, "turn") != 0 && strcmp(scope, "session") != 0) ||
	    !cJSON_IsObject(file_system)) {
		return tool_result_error_copy(
			result, "invalid_arguments",
			"permissions.file_system is required; scope must be turn or session.");
	}
	rc = (nb_vec_init(&requests, sizeof(*entry)),
	      nb_vec_reserve(&requests, 8) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = collect_requests(file_system, &requests, &failed_path);
	if (rc != 0) {
		rc = permission_error(result, rc, failed_path, 1);
		return rc;
	}
	qsort(((void *)requests.data), requests.len, requests.elem_size, request_parent_first);
	data = cJSON_CreateObject();
	cJSON *granted[sizeof(permissions) / sizeof(permissions[0])] = {0};

	if (!data || !cJSON_AddStringToObject(data, "scope", scope) ||
	    !cJSON_AddStringToObject(data, "principal", "*")) {
		rc = (NB_ERR_NOMEM);
		return rc;
	}
	for (size_t i = 0; i < sizeof(permissions) / sizeof(permissions[0]); i++) {
		granted[i] = cJSON_AddArrayToObject(data, permissions[i].name);
		if (!granted[i]) {
			rc = (NB_ERR_NOMEM);
			return rc;
		}
	}
	for (size_t _i_ = 0; _i_ < nb_vec_len(&requests) &&
			     (entry = (struct directory_request *)nb_vec_at(&requests, _i_));
	     _i_++) {
		struct tool_directory_access_request request = {
			.access = entry->access, .principal = "*", .command = command,
			.path = entry->path, .reason = reason,
			.session_scope = strcmp(scope, "session") == 0,
		};
		char resolved[PATH_MAX];

		rc = tool_context_request_directory_access(tctx, &request,
			resolved, sizeof(resolved));
		if (rc != 0) {
			rc = permission_error(result, rc, entry->path, 0);
			return rc;
		}
		for (size_t i = 0; i < sizeof(permissions) / sizeof(permissions[0]); i++) {
			if (!(entry->access & permissions[i].access))
				continue;
			cJSON *path = cJSON_CreateString(resolved);

			if (!path || !cJSON_AddItemToArray(granted[i], path)) {
				cJSON_Delete(path);
				rc = (NB_ERR_NOMEM);
				return rc;
			}
		}
	}
	rc = tool_result_success_take(result, &data);

	return rc;
}

int request_permissions_init(struct tool_registry *reg,
			     struct tool_context *tctx)
{
	static const char schema[] =
		"{\"type\":\"object\",\"properties\":{"
		"\"command\":{\"type\":\"string\",\"description\":"
		"\"Optional command context; grants apply to all tools and commands\"},"
		"\"permissions\":{\"type\":\"object\",\"properties\":{"
		"\"file_system\":{\"type\":\"object\",\"properties\":{"
		"\"read\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
		"\"write\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
		"\"delete\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"
		"},\"additionalProperties\":false}}},"
		"\"scope\":{\"type\":\"string\",\"enum\":[\"turn\",\"session\"],"
		"\"description\":\"Grant lifetime; defaults to turn\"},"
		"\"justification\":{\"type\":\"string\"}},"
		"\"required\":[\"permissions\"],"
		"\"additionalProperties\":false}";
	int rc;

	if (!reg || !tctx)
		return NB_ERR_INVAL;
	rc = tool_register_borrow(
		reg,
		&(struct tool_spec){
			.origin = TOOL_ORIGIN_BUILTIN,
			.name = "request_permissions",
			.description =
				"Request the smallest additional directory permissions for this "
				"workflow. "
				"Combine every anticipated read, write and delete permission for "
				"each "
				"directory in one call; approve each directory once for the listed "
				"access. "
				"Reuse existing grants rather than requesting operations one at a "
				"time. "
				"Read, write and delete grants apply to all tools and commands "
				"within the approved directories, for this turn by default or "
				"the session when scope=session. Command is optional context. "
				"Write does not include deletion; atomic replacement and "
				"installers may also need delete. Use scope=turn unless "
				"repeated operations need scope=session.",
			.input_schema = schema,
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.exec = request_permissions_run,
			.user_data = tctx,
		});
	if (rc == 0) {
		struct tool_entry *entry = tool_borrow(reg, "request_permissions");

		if (entry)
			entry->flags |= TOOL_FLAG_INTERNAL_APPROVAL;
	}
	return rc;
}
