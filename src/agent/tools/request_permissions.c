#include "request_permissions.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "util/error.h"
#include "cJSON.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int request_array(struct tool_context *tctx, const char *principal,
			 const char *command, cJSON *paths,
			 enum tool_path_op operation, cJSON *granted,
			 int session_scope,
			 struct tool_result *result)
{
	cJSON *path;

	if (!paths)
		return 0;
	if (!cJSON_IsArray(paths)) {
		int rc = tool_result_error(result, "invalid_arguments",
			"Permission paths must be arrays.");

		return rc == 0 ? 1 : rc;
	}
	cJSON_ArrayForEach(path, paths) {
		char resolved[PATH_MAX];
		int rc;

		if (!cJSON_IsString(path) || !path->valuestring) {
			rc = tool_result_error(result, "invalid_arguments",
				"Permission paths must contain strings.");
			return rc == 0 ? 1 : rc;
		}
		rc = tool_context_request_scoped_access(
			tctx, operation, principal, command, path->valuestring,
			session_scope, resolved, sizeof(resolved));
		if (rc != 0) {
			int result_rc = tool_result_error(
				result, rc == -EACCES ? "permission_denied" :
				"permission_request_failed",
				rc == -EACCES ? "Permission was denied by the user." :
				"The directory permission could not be granted.");

			return result_rc == 0 ? 1 : result_rc;
		}
		cJSON_AddItemToArray(granted, cJSON_CreateString(resolved));
	}
	return 0;
}

static int request_permissions_run(const char *args_json,
				   struct tool_result *result, void *user_data)
{
	struct tool_context *tctx = user_data;
	cJSON *root = args_json ? cJSON_Parse(args_json) : NULL;
	cJSON *permissions;
	cJSON *file_system;
	cJSON *data;
	cJSON *granted_write;
	cJSON *granted_delete;
	const char *command;
	const char *scope;
	char principal[TOOL_CONTEXT_CLI_NAME_MAX] = { 0 };
	int session_scope;
	int rc;

	if (!tctx || !root)
		MORPH_RETURN(-EINVAL);
	command = cJSON_GetStringValue(cJSON_GetObjectItem(root, "command"));
	if (!command || !*command) {
		cJSON_Delete(root);
		return tool_result_error(result, "invalid_arguments",
			"command is required so the grant can be scoped safely.");
	}
	if (tool_context_command_principal(command, principal,
					   sizeof(principal)) != 0)
		strncpy(principal, "shell", sizeof(principal) - 1);
	scope = cJSON_GetStringValue(cJSON_GetObjectItem(root, "scope"));
	if (!scope)
		scope = "turn";
	if (strcmp(scope, "turn") != 0 && strcmp(scope, "session") != 0) {
		cJSON_Delete(root);
		return tool_result_error(result, "invalid_arguments",
			"scope must be turn or session.");
	}
	session_scope = strcmp(scope, "session") == 0;
	permissions = cJSON_GetObjectItem(root, "permissions");
	file_system = cJSON_IsObject(permissions) ?
		cJSON_GetObjectItem(permissions, "file_system") : NULL;
	if (!cJSON_IsObject(file_system)) {
		cJSON_Delete(root);
		return tool_result_error(result, "invalid_arguments",
			"permissions.file_system is required.");
	}
	data = cJSON_CreateObject();
	granted_write = cJSON_CreateArray();
	granted_delete = cJSON_CreateArray();
	if (!data || !granted_write || !granted_delete) {
		cJSON_Delete(root);
		cJSON_Delete(data);
		cJSON_Delete(granted_write);
		cJSON_Delete(granted_delete);
		MORPH_RETURN(-ENOMEM);
	}
	rc = request_array(tctx, principal, command,
		cJSON_GetObjectItem(file_system, "write"), TOOL_PATH_WRITE,
		granted_write, session_scope, result);
	if (rc == 0)
		rc = request_array(tctx, principal, command,
			cJSON_GetObjectItem(file_system, "delete"),
			TOOL_PATH_DELETE, granted_delete, session_scope, result);
	if (rc != 0) {
		cJSON_Delete(root);
		cJSON_Delete(data);
		cJSON_Delete(granted_write);
		cJSON_Delete(granted_delete);
		return rc == 1 ? 0 : rc;
	}
	cJSON_AddStringToObject(data, "scope", scope);
	cJSON_AddStringToObject(data, "principal", principal);
	cJSON_AddItemToObject(data, "write", granted_write);
	cJSON_AddItemToObject(data, "delete", granted_delete);
	cJSON_Delete(root);
	return tool_result_success(result, data);
}

int request_permissions_init(struct tool_registry *reg,
			     struct tool_context *tctx)
{
	static const char schema[] =
		"{\"type\":\"object\",\"properties\":{"
		"\"command\":{\"type\":\"string\",\"description\":"
		"\"Exact shell command that will use the permissions\"},"
		"\"permissions\":{\"type\":\"object\",\"properties\":{"
		"\"file_system\":{\"type\":\"object\",\"properties\":{"
		"\"write\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
		"\"delete\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"
		"},\"additionalProperties\":false}}},"
		"\"scope\":{\"type\":\"string\",\"enum\":[\"turn\",\"session\"],"
		"\"description\":\"Grant lifetime; defaults to turn\"},"
		"\"justification\":{\"type\":\"string\"}},"
		"\"required\":[\"command\",\"permissions\"],"
		"\"additionalProperties\":false}";
	int rc;

	if (!reg || !tctx)
		MORPH_RETURN(-EINVAL);
	rc = tool_register(reg, &(struct tool_spec){
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "request_permissions",
		.description = "Request the smallest additional filesystem "
			"permissions needed by a later bash_exec command. Grants are "
			"scoped to that command executable for this turn by default, "
			"or for the session when scope=session.",
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
