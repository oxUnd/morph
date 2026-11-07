#include "request_permissions.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "util/error.h"
#include "util/buf.h"
#include "util/arena.h"
#include "util/array.h"
#include "util/json.h"

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

static int collect_requests(struct json *file_system, struct arena *arena, morph_array_t *requests,
			    const char **failed_path)
{
	for (size_t i = 0; i < sizeof(permissions) / sizeof(permissions[0]); i++) {
		struct json *paths = json_member(file_system, permissions[i].name);
		struct json *path;

		if (!paths)
			continue;
		if (!json_is_array_value(paths))
			return -EINVAL;
		json_foreach (path, paths) {
			char canonical[PATH_MAX];

			if (!json_is_string_value(path))
				return -EINVAL;
			*failed_path = json_string_value(path);
			/* Merge before validating a path created by the same write request. */
			int rc = tool_context_resolve_directory_access(
				json_string_value(path), TOOL_PATH_ACCESS_ALL, canonical,
				sizeof(canonical));

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
	struct json *root = args_json ? json_parse_ex(args_json, NULL, 1, NULL) : NULL;
	struct json *data = NULL;
	struct arena *arena = NULL;
	morph_array_t requests = {0};
	const char *failed_path = NULL;
	struct directory_request *entry;
	int rc;

	if (!tctx || !json_is_object_value(root)) {
		json_free(root);
		return tool_result_error(result, "invalid_arguments",
			"Expected a permission request.");
	}
	const char *command = json_string_value(json_member_ci(root, "command"));
	const char *scope = json_string_value(json_member_ci(root, "scope"));
	const char *reason = json_string_value(json_member_ci(root, "justification"));
	struct json *permission = json_member_ci(root, "permissions");
	struct json *file_system = json_member_ci(permission, "file_system");

	if (!command || !*command)
		command = "Access the requested directories";
	if (!scope)
		scope = "turn";
	if ((strcmp(scope, "turn") != 0 && strcmp(scope, "session") != 0) ||
	    !json_is_object_value(file_system)) {
		json_free(root);
		return tool_result_error(result, "invalid_arguments",
			"permissions.file_system is required; scope must be turn or session.");
	}
	arena = arena_create(0);
	if (!arena) {
		json_free(root);
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
	data = json_object();
	struct json *granted[sizeof(permissions) / sizeof(permissions[0])] = {0};

	if (!data || !json_add_string(data, "scope", scope) ||
	    !json_add_string(data, "principal", "*")) {
		rc = (-ENOMEM);
		goto out_free_request;
	}
	for (size_t i = 0; i < sizeof(permissions) / sizeof(permissions[0]); i++) {
		granted[i] = json_add_array(data, permissions[i].name);
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
			struct json *path = json_string(resolved);

			if (!path || json_array_add(granted[i], path) != 0) {
				json_free(path);
				rc = (-ENOMEM);
				goto out_free_request;
			}
		}
	}
	rc = tool_result_success(result, data);
	data = NULL;

out_free_request:
	json_free(data);
	morph_array_cleanup(&requests);
	arena_destroy(arena);
	json_free(root);
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
