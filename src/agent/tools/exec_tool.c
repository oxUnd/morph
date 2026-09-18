#include "exec_tool.h"
#include "agent/tool_context.h"
#include "config/config.h"
#include "exec/command_analyzer.h"
#include "exec/process.h"
#include "sandbox.h"
#include "util/error.h"
#include "util/id.h"
#include "cJSON.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define EXEC_DEFAULT_TIMEOUT_MS 120000u
#define EXEC_DEFAULT_YIELD_MS 10000u

struct exec_runtime {
	pthread_mutex_t mutex;
	int refs;
	struct process_manager *manager;
	struct tool_context *tool_context;
	uint64_t default_timeout_ms;
	uint64_t default_yield_time_ms;
	int network;
};

static void exec_runtime_destroy(void *opaque)
{
	struct exec_runtime *runtime = opaque;
	int destroy = 0;

	if (!runtime)
		return;
	pthread_mutex_lock(&runtime->mutex);
	runtime->refs--;
	if (runtime->refs == 0)
		destroy = 1;
	pthread_mutex_unlock(&runtime->mutex);
	if (!destroy)
		return;
	process_manager_destroy(runtime->manager);
	pthread_mutex_destroy(&runtime->mutex);
	free(runtime);
}

static int exec_runtime_create(struct tool_context *tctx,
			       const struct config_exec *exec_config,
			       struct exec_runtime **out)
{
	struct process_manager_config config = {
		.max_session_output = exec_config ?
			(size_t)exec_config->max_session_output : 1024u * 1024u,
		.max_inline_output = exec_config ?
			(size_t)exec_config->max_inline_output : 32u * 1024u,
		.kill_grace_ms = exec_config ?
			(uint64_t)exec_config->kill_grace_ms : 500u,
		.shell = exec_config ? exec_config->shell : "/bin/bash",
	};
	struct exec_runtime *runtime;
	int rc;

	if (!out)
		MORPH_RETURN(-EINVAL);
	*out = NULL;
	runtime = calloc(1, sizeof(*runtime));
	if (!runtime)
		MORPH_RETURN(-ENOMEM);
	runtime->refs = 2;
	runtime->tool_context = tctx;
	runtime->default_timeout_ms = exec_config ?
		(uint64_t)exec_config->default_timeout_ms : EXEC_DEFAULT_TIMEOUT_MS;
	runtime->default_yield_time_ms = exec_config ?
		(uint64_t)exec_config->yield_time_ms : EXEC_DEFAULT_YIELD_MS;
	runtime->network = exec_config ? exec_config->network : 0;
	rc = pthread_mutex_init(&runtime->mutex, NULL);
	if (rc != 0) {
		free(runtime);
		MORPH_RETURN(-rc);
	}
	rc = process_manager_create(&config, &runtime->manager);
	if (rc != 0) {
		pthread_mutex_destroy(&runtime->mutex);
		free(runtime);
		return rc;
	}
	*out = runtime;
	return 0;
}

static int json_bool(const cJSON *root, const char *name, int fallback)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

	return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static uint64_t json_u64(const cJSON *root, const char *name,
				 uint64_t fallback)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

	if (!cJSON_IsNumber(item) || item->valuedouble < 0)
		return fallback;
	return (uint64_t)item->valuedouble;
}

static const char *json_string(const cJSON *root, const char *name)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

	return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int policy_check(struct exec_runtime *runtime, const char *command,
			const char *workdir, const char *args_json,
			struct tool_result *result)
{
	struct command_analysis analysis;
	int rc;

	rc = command_analyze(command, &analysis);
	if (rc != 0)
		return rc;
	if (analysis.count == 0 || analysis.complex) {
		command_analysis_cleanup(&analysis);
		(void)tool_result_error(result, "approval_required",
			"complex shell syntax requires approval");
		return -EPERM;
	}
	if (runtime->tool_context) {
		for (size_t i = 0; i < analysis.count; i++) {
			struct command_segment *segment = &analysis.segments[i];
			struct tool_operation operation = {
				.kind = TOOL_OP_COMMAND,
				.tool_name = "exec",
				.principal = segment->argc > 0 ? segment->argv[0] : "shell",
				.action = segment->raw,
				.scope = workdir,
				.details_json = args_json,
			};
			enum tool_operation_verdict verdict = TOOL_OP_DENY;

			rc = tool_context_check_operation_verdict(
				runtime->tool_context, &operation, &verdict);
			if (rc != 0 || verdict == TOOL_OP_DENY) {
				command_analysis_cleanup(&analysis);
				(void)tool_result_error(result,
					rc == -EACCES ? "permission_denied" :
					"approval_required",
					verdict == TOOL_OP_DENY ?
					"command requires approval" :
					"command is not allowed by policy");
				return rc != 0 ? rc : -EPERM;
			}
		}
	}
	command_analysis_cleanup(&analysis);
	return 0;
}

static void snapshot_add_json(cJSON *object,
			      const struct process_snapshot *snapshot)
{
	cJSON_AddStringToObject(object, "status",
		process_state_name(snapshot->state));
	cJSON_AddStringToObject(object, "stdout",
		snapshot->stdout_text ? snapshot->stdout_text : "");
	cJSON_AddStringToObject(object, "stderr",
		snapshot->stderr_text ? snapshot->stderr_text : "");
	cJSON_AddBoolToObject(object, "stdout_truncated",
		snapshot->stdout_truncated);
	cJSON_AddBoolToObject(object, "stderr_truncated",
		snapshot->stderr_truncated);
	cJSON_AddNumberToObject(object, "stdout_omitted_bytes",
		(double)snapshot->stdout_omitted_bytes);
	cJSON_AddNumberToObject(object, "stderr_omitted_bytes",
		(double)snapshot->stderr_omitted_bytes);
	cJSON_AddNumberToObject(object, "duration_ms",
		(double)snapshot->duration_ms);
	if (snapshot->state == PROCESS_EXITED)
		cJSON_AddNumberToObject(object, "exit_code", snapshot->exit_code);
	if (snapshot->signal_number > 0)
		cJSON_AddNumberToObject(object, "signal", snapshot->signal_number);
}

static int result_snapshot(struct tool_result *result,
			   const char *session_id, const char *command,
			   const char *workdir,
			   const struct process_snapshot *snapshot)
{
	cJSON *data = cJSON_CreateObject();
	int rc;

	if (!data)
		MORPH_RETURN(-ENOMEM);
	cJSON_AddStringToObject(data, "session_id", session_id);
	if (command)
		cJSON_AddStringToObject(data, "command", command);
	if (workdir)
		cJSON_AddStringToObject(data, "workdir", workdir);
	snapshot_add_json(data, snapshot);
	rc = tool_result_success(result, data);
	if (rc != 0)
		cJSON_Delete(data);
	return rc;
}

static int exec_run(const char *args_json, struct tool_result *result,
			void *user_data)
{
	struct exec_runtime *runtime = user_data;
	cJSON *root;
	const char *command;
	const char *workdir;
	struct process_spawn_options options;
	struct sandbox_config sandbox;
	char resolved_workdir[PATH_MAX];
	char principal[TOOL_CONTEXT_CLI_NAME_MAX] = "shell";
	const char *write_grants[TOOL_CONTEXT_ALLOW_MAX];
	const char *delete_grants[TOOL_CONTEXT_ALLOW_MAX];
	char *read_paths[10];
	char *write_paths[TOOL_CONTEXT_ALLOW_MAX + 2];
	char *delete_paths[TOOL_CONTEXT_ALLOW_MAX + 1];
	int read_path_count = 0;
	int write_path_count = 0;
	int delete_path_count = 0;
	struct process_snapshot snapshot;
	char session_id[PROCESS_SESSION_ID_MAX];
	int rc;

	if (!runtime || !result)
		MORPH_RETURN(-EINVAL);
	root = args_json ? cJSON_Parse(args_json) : NULL;
	if (!root) {
		(void)tool_result_error(result, "invalid_request",
			"exec arguments must be a JSON object");
		MORPH_RETURN(-EINVAL);
	}
	command = json_string(root, "command");
	if (!command)
		command = json_string(root, "cmd");
	workdir = json_string(root, "workdir");
	if (!workdir)
		workdir = json_string(root, "cwd");
	if (!command || !*command) {
		cJSON_Delete(root);
		(void)tool_result_error(result, "invalid_request",
			"command is required");
		MORPH_RETURN(-EINVAL);
	}
	if (!workdir && runtime->tool_context)
		workdir = tool_context_workdir(runtime->tool_context);
	if (runtime->tool_context)
		(void)tool_context_command_principal(command, principal,
						     sizeof(principal));
	rc = policy_check(runtime, command, workdir, args_json, result);
	if (rc != 0) {
		cJSON_Delete(root);
		return rc;
	}
	memset(&options, 0, sizeof(options));
	options.command = command;
	options.workdir = workdir;
	options.timeout_ms = json_u64(root, "timeout_ms",
		runtime->default_timeout_ms);
	options.yield_time_ms = json_u64(root, "yield_time_ms",
		runtime->default_yield_time_ms);
	options.pty = json_bool(root, "pty", 0);
	options.background = json_bool(root, "background", 0);
	memset(&sandbox, 0, sizeof(sandbox));
	if (workdir && realpath(workdir, resolved_workdir)) {
		read_paths[read_path_count++] = resolved_workdir;
		write_paths[write_path_count++] = resolved_workdir;
		delete_paths[delete_path_count++] = resolved_workdir;
	} else {
		write_paths[write_path_count++] =
			(char *)(workdir ? workdir : ".");
		delete_paths[delete_path_count++] =
			(char *)(workdir ? workdir : ".");
	}
	read_paths[read_path_count++] = "/usr";
	read_paths[read_path_count++] = "/bin";
	read_paths[read_path_count++] = "/sbin";
	read_paths[read_path_count++] = "/System";
	read_paths[read_path_count++] = "/Library";
	read_paths[read_path_count++] = "/opt/homebrew";
	read_paths[read_path_count++] = "/private";
	read_paths[read_path_count++] = "/tmp";
	write_paths[write_path_count++] = "/tmp";
	delete_paths[delete_path_count++] = "/tmp";
	if (runtime->tool_context) {
		int count = tool_context_collect_write_grants(
			runtime->tool_context, principal, write_grants,
			TOOL_CONTEXT_ALLOW_MAX);

		for (int i = 0; i < count; i++)
			write_paths[write_path_count++] = (char *)write_grants[i];
		count = tool_context_collect_delete_grants(
			runtime->tool_context, principal, delete_grants,
			TOOL_CONTEXT_ALLOW_MAX);
		for (int i = 0; i < count; i++)
			delete_paths[delete_path_count++] = (char *)delete_grants[i];
	}
	sandbox.path_policy_enabled = 1;
	sandbox.read_paths = read_paths;
	sandbox.read_paths_count = read_path_count;
	sandbox.write_paths = write_paths;
	sandbox.write_paths_count = write_path_count;
	sandbox.delete_paths = delete_paths;
	sandbox.delete_paths_count = delete_path_count;
	sandbox.network_access = runtime->network;
	sandbox.process_exec = 1;
	sandbox.allow_pty = options.pty;
	sandbox.allow_process_info = 1;
	sandbox.allow_temp = 1;
	options.sandbox = &sandbox;
	memset(&snapshot, 0, sizeof(snapshot));
	rc = process_spawn(runtime->manager, &options, session_id, &snapshot);
	if (rc != 0) {
		const char *code = rc == -ENOENT ? "invalid_workdir" :
			rc == -ENOTDIR ? "invalid_workdir" : "spawn_failed";

		cJSON_Delete(root);
		(void)tool_result_error(result, code, strerror(-rc));
		process_snapshot_cleanup(&snapshot);
		return rc;
	}
	rc = result_snapshot(result, session_id, command, workdir, &snapshot);
	process_snapshot_cleanup(&snapshot);
	cJSON_Delete(root);
	return rc;
}

static int process_run(const char *args_json, struct tool_result *result,
			   void *user_data)
{
	struct exec_runtime *runtime = user_data;
	cJSON *root;
	const char *session_id;
	const char *action;
	const char *input;
	struct process_snapshot snapshot;
	int rc;

	if (!runtime || !result)
		MORPH_RETURN(-EINVAL);
	root = args_json ? cJSON_Parse(args_json) : NULL;
	if (!root) {
		(void)tool_result_error(result, "invalid_request",
			"process arguments must be a JSON object");
		MORPH_RETURN(-EINVAL);
	}
	session_id = json_string(root, "session_id");
	action = json_string(root, "action");
	input = json_string(root, "input");
	if (!session_id || !action) {
		cJSON_Delete(root);
		(void)tool_result_error(result, "invalid_request",
			"session_id and action are required");
		MORPH_RETURN(-EINVAL);
	}
	if (strcmp(action, "write") == 0)
		rc = process_session_write(runtime->manager, session_id,
			input ? input : "", input ? strlen(input) : 0);
	else if (strcmp(action, "interrupt") == 0)
		rc = process_session_interrupt(runtime->manager, session_id);
	else if (strcmp(action, "kill") == 0)
		rc = process_session_kill(runtime->manager, session_id);
	else if (strcmp(action, "poll") == 0)
		rc = 0;
	else
		rc = -EINVAL;
	if (rc != 0) {
		const char *code = rc == -ENOENT ? "process_not_found" :
			rc == -EPIPE ? "process_not_running" :
			rc == -EAGAIN ? "io_error" : "invalid_request";

		cJSON_Delete(root);
		(void)tool_result_error(result, code, strerror(-rc));
		return rc;
	}
	memset(&snapshot, 0, sizeof(snapshot));
	rc = process_session_snapshot(runtime->manager, session_id, &snapshot);
	if (rc == 0)
		rc = result_snapshot(result, session_id, NULL, NULL, &snapshot);
	process_snapshot_cleanup(&snapshot);
	cJSON_Delete(root);
	return rc;
}

int exec_tool_init(struct tool_registry *reg, struct tool_context *tctx,
		   const struct config_exec *config)
{
	struct exec_runtime *runtime;
	struct tool_spec exec_spec;
	struct tool_spec process_spec;
	int rc;

	if (!reg)
		MORPH_RETURN(-EINVAL);
	rc = exec_runtime_create(tctx, config, &runtime);
	if (rc != 0)
		return rc;
	memset(&exec_spec, 0, sizeof(exec_spec));
	exec_spec.origin = TOOL_ORIGIN_BUILTIN;
	exec_spec.name = "exec";
	exec_spec.title = "Execute command";
	exec_spec.description = "Run a shell command with workdir, policy, timeout, yield, and process session support. If the result reports error.code=sandbox_denied, request the smallest additional capability and retry the exact command.";
	exec_spec.input_schema = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"workdir\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\",\"minimum\":0},\"yield_time_ms\":{\"type\":\"integer\",\"minimum\":0},\"pty\":{\"type\":\"boolean\"},\"background\":{\"type\":\"boolean\"},\"cmd\":{\"type\":\"string\"}},\"required\":[\"command\"],\"additionalProperties\":false}";
	exec_spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	exec_spec.exec = exec_run;
	exec_spec.user_data = runtime;
	exec_spec.user_data_destroy = exec_runtime_destroy;
	exec_spec.flags = TOOL_FLAG_INTERNAL_APPROVAL;
	rc = tool_register(reg, &exec_spec);
	if (rc != 0) {
		exec_runtime_destroy(runtime);
		return rc;
	}
	memset(&process_spec, 0, sizeof(process_spec));
	process_spec.origin = TOOL_ORIGIN_BUILTIN;
	process_spec.name = "process";
	process_spec.title = "Manage process";
	process_spec.description = "Poll, write to, interrupt, or kill an exec process session.";
	process_spec.input_schema = "{\"type\":\"object\",\"properties\":{\"session_id\":{\"type\":\"string\"},\"action\":{\"type\":\"string\",\"enum\":[\"poll\",\"write\",\"interrupt\",\"kill\"]},\"input\":{\"type\":\"string\"}},\"required\":[\"session_id\",\"action\"],\"additionalProperties\":false}";
	process_spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	process_spec.exec = process_run;
	process_spec.user_data = runtime;
	process_spec.user_data_destroy = exec_runtime_destroy;
	process_spec.flags = TOOL_FLAG_INTERNAL_APPROVAL;
	rc = tool_register(reg, &process_spec);
	if (rc != 0) {
		/* The exec registration owns the other runtime reference. */
		exec_runtime_destroy(runtime);
		return rc;
	}
	return 0;
}
