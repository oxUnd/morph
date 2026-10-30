#include "request_permissions.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "util/error.h"
#include "util/buf.h"
#include "util/arena.h"
#include "util/array.h"
#include "cJSON.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct directory_request {
	const char *path;
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

static int collect_requests(cJSON *file_system, struct arena *arena,
	morph_array_t *requests, const char **failed_path)
{
	for (size_t i = 0; i < sizeof(permissions) / sizeof(permissions[0]); i++) {
		cJSON *paths = cJSON_GetObjectItemCaseSensitive(file_system, permissions[i].name);
		cJSON *path;

		if (!paths)
			continue;
		if (!cJSON_IsArray(paths))
			return -EINVAL;
		cJSON_ArrayForEach(path, paths) {
			char canonical[PATH_MAX];

			if (!cJSON_IsString(path))
				return -EINVAL;
			*failed_path = path->valuestring;
			/* Merge before validating a path created by the same write request. */
			int rc = tool_context_resolve_directory_access(path->valuestring,
				TOOL_PATH_ACCESS_ALL, canonical, sizeof(canonical));

			if (rc != 0)
				return rc;
			struct directory_request *entry;
			int found = 0;

			morph_array_foreach(entry, requests, struct directory_request) {
				if (strcmp(entry->path, canonical) == 0) {
					entry->access |= permissions[i].access;
					found = 1;
					break;
				}
			}
			if (found)
				continue;
			const char *copy = arena_strdup(arena, canonical);

			entry = copy ? morph_array_push(requests) : NULL;
			if (!entry)
				return -ENOMEM;
			entry->path = copy;
			entry->access = permissions[i].access;
		}
	}
	struct directory_request *entry;

	/* Validate every directory before asking the user about any of them. */
	morph_array_foreach(entry, requests, struct directory_request) {
		char canonical[PATH_MAX];

		*failed_path = entry->path;
		int rc = tool_context_resolve_directory_access(entry->path, entry->access,
			canonical, sizeof(canonical));

		if (rc != 0)
			return rc;
	}
	return 0;
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
	const char *code = validation ? "invalid_arguments" :
		error == -EACCES ? "permission_denied" :
		error == -EPERM ? "approval_unavailable" : "permission_request_failed";

	return tool_result_errorf(result, code, "Directory permission request failed%s%s: %s",
		path ? " for " : "", path ? path : "", morph_strerror(error));
}

static int request_permissions_run(const char *args_json,
	struct tool_result *result, void *user_data)
{
	struct tool_context *tctx = user_data;
	cJSON *root = args_json ? cJSON_ParseWithOpts(args_json, NULL, 1) : NULL;
	cJSON *data = NULL;
	struct arena *arena = NULL;
	morph_array_t requests = {0};
	const char *failed_path = NULL;
	struct directory_request *entry;
	int rc;

	if (!tctx || !cJSON_IsObject(root)) {
		cJSON_Delete(root);
		return tool_result_error(result, "invalid_arguments",
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
		cJSON_Delete(root);
		return tool_result_error(result, "invalid_arguments",
			"permissions.file_system is required; scope must be turn or session.");
	}
	arena = arena_create(0);
	if (!arena) {
		cJSON_Delete(root);
		return -ENOMEM;
	}
	rc = morph_array_init(&requests, 8, sizeof(*entry));
	if (rc != 0)
		goto out_free_request;
	rc = collect_requests(file_system, arena, &requests, &failed_path);
	if (rc != 0) {
		rc = permission_error(result, rc, failed_path, 1);
		goto out_free_request;
	}
	qsort(requests.elts, requests.nelts, requests.size, request_parent_first);
	data = cJSON_CreateObject();
	cJSON *granted[sizeof(permissions) / sizeof(permissions[0])] = {0};

	if (!data || !cJSON_AddStringToObject(data, "scope", scope) ||
	    !cJSON_AddStringToObject(data, "principal", "*")) {
		rc = (-ENOMEM);
		goto out_free_request;
	}
	for (size_t i = 0; i < sizeof(permissions) / sizeof(permissions[0]); i++) {
		granted[i] = cJSON_AddArrayToObject(data, permissions[i].name);
		if (!granted[i]) {
			rc = (-ENOMEM);
			goto out_free_request;
		}
	}
	morph_array_foreach(entry, &requests, struct directory_request) {
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
			goto out_free_request;
		}
		for (size_t i = 0; i < sizeof(permissions) / sizeof(permissions[0]); i++) {
			if (!(entry->access & permissions[i].access))
				continue;
			cJSON *path = cJSON_CreateString(resolved);

			if (!path || !cJSON_AddItemToArray(granted[i], path)) {
				cJSON_Delete(path);
				rc = (-ENOMEM);
				goto out_free_request;
			}
		}
	}
	rc = tool_result_success(result, data);
	data = NULL;

out_free_request:
	cJSON_Delete(data);
	morph_array_cleanup(&requests);
	arena_destroy(arena);
	cJSON_Delete(root);
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
		return -EINVAL;
	rc = tool_register(reg, &(struct tool_spec){
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "request_permissions",
		.description = "Request the smallest additional directory permissions for this workflow. "
			"Combine every anticipated read, write and delete permission for each "
			"directory in one call; approve each directory once for the listed access. "
			"Reuse existing grants rather than requesting operations one at a time. "
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
		struct tool_entry *entry = tool_lookup(reg, "request_permissions");

		if (entry)
			entry->flags |= TOOL_FLAG_INTERNAL_APPROVAL;
	}
	return rc;
}
