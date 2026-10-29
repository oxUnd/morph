#include "exec_tool.h"
#include "exec_jobs.h"
#include "agent/tool_runtime.h"
#include "http/client.h"
#include "agent/environment_context.h"
#include "agent/tool_context.h"
#include "config/config.h"
#include "exec/process.h"
#include "sandbox.h"
#include "sandbox/tool_paths.h"
#include "path_policy.h"
#include "util/bash_parse.h"
#include "util/error.h"
#include "util/id.h"
#include "cJSON.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define EXEC_DEFAULT_TIMEOUT_MS 120000u
#define EXEC_DEFAULT_YIELD_MS 10000u

struct exec_runtime {
	pthread_mutex_t mutex;
	int refs;
	struct process_manager *manager;
	struct exec_jobs *jobs;
	struct tool_context *tool_context;
	uint64_t default_timeout_ms;
	uint64_t default_yield_time_ms;
	int network;
	int allow_host_execution;
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
	exec_jobs_destroy(runtime->jobs);
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
	runtime->allow_host_execution = exec_config ? exec_config->allow_host_execution : 0;
	rc = pthread_mutex_init(&runtime->mutex, NULL);
	if (rc != 0) {
		free(runtime);
		MORPH_RETURN(-rc);
	}
	rc = exec_jobs_create(&runtime->jobs);
	if (rc != 0) {
		pthread_mutex_destroy(&runtime->mutex);
		free(runtime);
		return rc;
	}
	config.completed = exec_jobs_complete;
	config.completed_user_data = runtime->jobs;
	rc = process_manager_create(&config, &runtime->manager);
	if (rc != 0) {
		exec_jobs_destroy(runtime->jobs);
		pthread_mutex_destroy(&runtime->mutex);
		free(runtime);
		return rc;
	}
	exec_jobs_bind(runtime->jobs, runtime->manager, NULL, NULL);
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

	if (!cJSON_IsNumber(item) || item->valuedouble < 0 ||
	    item->valuedouble >= (double)UINT64_MAX)
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
			int host, struct tool_result *result)
{
	struct bash_parse_result analysis;
	const char **programs = NULL;
	int programs_count = 0;
	int dynamic = 0;
	int rc;

	rc = bash_parse_analyze(command, &analysis);
	if (rc != 0)
		return rc;
	if (analysis.has_error) {
		bash_parse_result_cleanup(&analysis);
		(void)tool_result_error(result, "approval_required",
			"shell command could not be analyzed safely");
		return -EPERM;
	}
	if (runtime->tool_context) {
		if (analysis.commands.nelts >= (size_t)INT_MAX) {
			bash_parse_result_cleanup(&analysis);
			MORPH_RETURN(-E2BIG);
		}
		programs = calloc(analysis.commands.nelts + 1, sizeof(*programs));
		if (!programs) {
			bash_parse_result_cleanup(&analysis);
			MORPH_RETURN(-ENOMEM);
		}
		for (size_t i = 0; i < analysis.commands.nelts; i++) {
			const struct bash_parse_command *parsed =
				morph_array_get(&analysis.commands, i);
			int duplicate = 0;

			if (!parsed || !parsed->name[0]) {
				dynamic = 1;
				continue;
			}
			for (int j = 0; j < programs_count; j++) {
				if (strcmp(programs[j], parsed->name) == 0) {
					duplicate = 1;
					break;
				}
			}
			if (!duplicate) {
				programs[programs_count] = parsed->name;
				programs_count++;
			}
		}
		if (dynamic || programs_count == 0) {
			int found = 0;

			for (int i = 0; i < programs_count; i++)
				found |= strcmp(programs[i], "shell") == 0;
			if (!found)
				programs[programs_count++] = "shell";
		}
		{
			struct tool_operation operation = {
				.kind = TOOL_OP_COMMAND,
				.tool_name = "exec",
				.principal = programs_count == 1 ?
					programs[0] : "shell",
				.action = command,
				.scope = workdir,
				.details_json = args_json,
				.reason = host ?
					"Host execution without the child OS sandbox was requested." :
					dynamic ? "The script contains runtime-expanded commands." : NULL,
				.programs = programs,
				.programs_count = programs_count,
			};
			enum tool_operation_verdict verdict = TOOL_OP_DENY;

			rc = tool_context_check_operation_verdict(
				runtime->tool_context, &operation, &verdict);
			free(programs);
			if (rc != 0 || verdict == TOOL_OP_DENY) {
				const char *code = rc == -EACCES ?
					"permission_denied" :
					rc == -EPERM ?
					"approval_unavailable" :
					"approval_required";
				const char *message = rc == -EACCES ?
					"Command approval was denied by the user." :
					rc == -EPERM ?
					"Interactive command approval is unavailable." :
					"Command approval failed.";

				bash_parse_result_cleanup(&analysis);
				(void)tool_result_error(result, code, message);
				return rc != 0 ? rc : -EPERM;
			}
		}
	}
	bash_parse_result_cleanup(&analysis);
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
			   const struct process_snapshot *snapshot,
			   const char *execution_mode)
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
	if (execution_mode)
		cJSON_AddStringToObject(data, "execution_mode", execution_mode);
	snapshot_add_json(data, snapshot);
	{
		char cursor[96];
		snprintf(cursor, sizeof(cursor), "%zu:%zu", snapshot->next_cursor.stdout_offset,
			 snapshot->next_cursor.stderr_offset);
		cJSON_AddStringToObject(data, "next_cursor", cursor);
		if (snapshot->stdout_path[0])
			cJSON_AddStringToObject(data, "stdout_path", snapshot->stdout_path);
		if (snapshot->stderr_path[0])
			cJSON_AddStringToObject(data, "stderr_path", snapshot->stderr_path);
	}
	rc = tool_result_success(result, data);
	if (rc != 0)
		cJSON_Delete(data);
	return rc;
}

/*
 * A linked git worktree stores its metadata in the main repository's
 * .git directory.  That path sits outside the workdir, so grant it
 * explicitly when workdir's .git is a file pointing elsewhere.  The
 * shared object store lives in the common dir, which we resolve
 * through the gitdir's "commondir" file.
 */
static void exec_resolve_worktree_git_dir(const char *workdir, char *out,
					  size_t out_size)
{
	char git_path[PATH_MAX];
	char line[PATH_MAX];
	char candidate[PATH_MAX];
	char resolved[PATH_MAX];
	const char *gitdir;
	struct stat st;
	FILE *file;

	if (out_size > 0)
		out[0] = '\0';
	if (!workdir || !*workdir || out_size == 0)
		return;
	if (snprintf(git_path, sizeof(git_path), "%s/.git", workdir) <= 0 ||
	    strlen(git_path) >= sizeof(git_path))
		return;
	if (stat(git_path, &st) != 0 || !S_ISREG(st.st_mode))
		return;
	file = fopen(git_path, "r");
	if (!file)
		return;
	if (!fgets(line, sizeof(line), file)) {
		fclose(file);
		return;
	}
	fclose(file);
	if (strncmp(line, "gitdir:", 7) != 0)
		return;
	gitdir = line + 7;
	while (*gitdir == ' ' || *gitdir == '\t')
		gitdir++;
	if (snprintf(candidate, sizeof(candidate), "%s", gitdir) <= 0)
		return;
	{
		size_t length = strlen(candidate);

		while (length > 0 && (candidate[length - 1] == '\n' ||
				      candidate[length - 1] == '\r'))
			candidate[--length] = '\0';
	}
	if (candidate[0] == '/') {
		if (!realpath(candidate, resolved))
			return;
	} else if (snprintf(git_path, sizeof(git_path), "%s/%s", workdir,
			    candidate) <= 0 || !realpath(git_path, resolved)) {
		return;
	}
	if (snprintf(git_path, sizeof(git_path), "%s/commondir",
		     resolved) > 0) {
		file = fopen(git_path, "r");
		if (file) {
			char common[PATH_MAX];
			char common_path[PATH_MAX];
			char common_resolved[PATH_MAX];
			size_t length;

			if (fgets(common, sizeof(common), file)) {
				length = strlen(common);
				while (length > 0 &&
				       (common[length - 1] == '\n' ||
					common[length - 1] == '\r'))
					common[--length] = '\0';
				if (common[0] == '/') {
					snprintf(common_path,
						 sizeof(common_path), "%s",
						 common);
				} else if (snprintf(common_path,
						    sizeof(common_path), "%s/%s",
						    resolved, common) <= 0) {
					common_path[0] = '\0';
				}
				if (common_path[0] &&
				    realpath(common_path, common_resolved))
					snprintf(resolved, sizeof(resolved),
						 "%s", common_resolved);
			}
			fclose(file);
		}
	}
	snprintf(out, out_size, "%s", resolved);
}

static int merge_path_grants(struct tool_context *tctx, const char *principal,
	struct sandbox_config *cfg, struct arena *arena)
{
	if (!tctx)
		return 0;
	for (int deletion = 0; deletion < 2; deletion++) {
		morph_array_t grants;
		enum tool_path_op op = deletion ? TOOL_PATH_DELETE : TOOL_PATH_WRITE;
		char **old = deletion ? cfg->delete_paths : cfg->write_paths;
		int old_count = deletion ? cfg->delete_paths_count : cfg->write_paths_count;
		int rc = morph_array_init(&grants, 16, sizeof(const char *));

		if (rc != 0)
			return rc;
		rc = tool_context_collect_path_grants(tctx, principal, op, &grants);
		if (rc == 0 && grants.nelts > (size_t)(INT_MAX - old_count))
			rc = -EOVERFLOW;
		if (rc != 0) {
			morph_array_cleanup(&grants);
			return rc;
		}
		size_t count = (size_t)old_count + grants.nelts;
		char **merged = arena_alloc(arena, count * sizeof(*merged));

		if (!merged) {
			morph_array_cleanup(&grants);
			MORPH_RETURN(-ENOMEM);
		}
		memcpy(merged, old, (size_t)old_count * sizeof(*merged));
		memcpy(merged + old_count, grants.elts, grants.nelts * sizeof(*merged));
		morph_array_cleanup(&grants);
		if (deletion) {
			cfg->delete_paths = merged;
			cfg->delete_paths_count = (int)count;
		} else {
			cfg->write_paths = merged;
			cfg->write_paths_count = (int)count;
		}
	}
	return 0;
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
	struct arena *path_arena;
	char resolved_workdir[PATH_MAX];
	char git_dir[PATH_MAX] = {0};
	char principal[TOOL_CONTEXT_CLI_NAME_MAX] = "shell";
	char *read_paths[12];
	char *write_paths[4];
	char *delete_paths[4];
	int read_path_count = 0;
	int write_path_count = 0;
	int delete_path_count = 0;
	struct process_snapshot snapshot;
	char session_id[PROCESS_SESSION_ID_MAX];
	int host;
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
	host = json_bool(root, "host", 0);
	if (host && !runtime->allow_host_execution) {
		cJSON_Delete(root);
		(void)tool_result_error(result, "host_execution_disabled",
			"Host execution requires exec.allow_host_execution=true in the user config.");
		MORPH_RETURN(-EACCES);
	}
	if (!workdir && runtime->tool_context)
		workdir = tool_context_workdir(runtime->tool_context);
	if (runtime->tool_context)
		(void)tool_context_command_principal(command, principal,
						     sizeof(principal));
	rc = policy_check(runtime, command, workdir, args_json, host, result);
	if (rc != 0) {
		cJSON_Delete(root);
		return rc;
	}
	memset(&options, 0, sizeof(options));
	options.command = command;
	options.workdir = workdir;
	options.background = json_bool(root, "background", json_bool(root, "service", 0));
	options.host_execution = host;
	options.timeout_ms = json_u64(root, "timeout_ms",
		options.background ? 0 : runtime->default_timeout_ms);
	options.yield_time_ms = json_u64(root, "yield_time_ms",
		runtime->default_yield_time_ms);
	options.pty = json_bool(root, "pty", 0);
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
	exec_resolve_worktree_git_dir(workdir, git_dir, sizeof(git_dir));
	if (git_dir[0]) {
		write_paths[write_path_count++] = git_dir;
		delete_paths[delete_path_count++] = git_dir;
	}
	sandbox.path_policy_enabled = 1;
	sandbox.read_all = 1;
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
	sandbox.allow_ipc = 1;
	sandbox.development_runtime = 1;
	path_arena = arena_create(0);

	if (!path_arena) {
		cJSON_Delete(root);
		(void)tool_result_error(result, "sandbox_policy_failed",
			morph_strerror(-ENOMEM));
		MORPH_RETURN(-ENOMEM);
	}
	rc = merge_path_grants(runtime->tool_context, principal, &sandbox, path_arena);
	if (rc == 0)
		rc = sandbox_prepare_tool_paths(&sandbox, path_arena);
	if (rc != 0) {
		arena_destroy(path_arena);
		cJSON_Delete(root);
		(void)tool_result_error(result, "sandbox_policy_failed",
			morph_strerror(rc));
		return rc;
	}
	sandbox.logger = runtime->tool_context ? runtime->tool_context->logger : NULL;
	options.logger = sandbox.logger;
	options.sandbox = host ? NULL : &sandbox;
	memset(&snapshot, 0, sizeof(snapshot));
	rc = process_spawn(runtime->manager, &options, session_id, &snapshot);
	arena_destroy(path_arena);
	if (rc != 0) {
		const char *code = rc == -ENOENT ? "invalid_workdir" :
			rc == -ENOTDIR ? "invalid_workdir" : "spawn_failed";

		cJSON_Delete(root);
		(void)tool_result_error(result, code, strerror(-rc));
		process_snapshot_cleanup(&snapshot);
		return rc;
	}
	if (snapshot.state < PROCESS_EXITED) {
		const struct tool_runtime_context *context = result->runtime;
		rc = exec_jobs_add(runtime->jobs, session_id, command,
			json_string(root, "description"), workdir,
			context ? context->memory_session_id : 0,
			context ? context->turn_id : NULL, json_bool(root, "service", 0),
			json_bool(root, "notify_only", 0));
		if (rc != 0) {
			(void)process_session_kill(runtime->manager, session_id);
			process_snapshot_cleanup(&snapshot);
			cJSON_Delete(root);
			return rc;
		}
	}
	rc = result_snapshot(result, session_id, command, workdir, &snapshot,
		host ? "host" : "sandbox");
	process_snapshot_cleanup(&snapshot);
	cJSON_Delete(root);
	return rc;
}

static int process_wait_cancelled(void *opaque)
{
	const struct tool_runtime_context *runtime = opaque;

	return http_wait_cancelable(runtime ? &runtime->http : NULL, 0) < 0;
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
	if (action && !strcmp(action, "list")) {
		cJSON *jobs = NULL;
		int64_t owner = result->runtime ? result->runtime->memory_session_id : 0;
		rc = exec_jobs_list(runtime->jobs, owner, &jobs);
		cJSON_Delete(root);
		if (rc) return rc;
		cJSON *data = cJSON_CreateObject();
		if (!data) { cJSON_Delete(jobs); MORPH_RETURN(-ENOMEM); }
		cJSON_AddItemToObject(data, "jobs", jobs);
		rc = tool_result_success(result, data);
		if (rc) cJSON_Delete(data);
		return rc;
	}
	if (!session_id || !action) {
		cJSON_Delete(root);
		(void)tool_result_error(result, "invalid_request",
			"session_id and action are required");
		MORPH_RETURN(-EINVAL);
	}
	rc = exec_jobs_authorize(runtime->jobs, session_id,
		result->runtime ? result->runtime->memory_session_id : 0);
	if (rc) {
		cJSON_Delete(root);
		(void)tool_result_error(result, "process_not_found", "Process not found in this session.");
		return rc;
	}
	if (strcmp(action, "write") == 0)
		rc = process_session_write(runtime->manager, session_id,
			input ? input : "", input ? strlen(input) : 0);
	else if (strcmp(action, "interrupt") == 0)
		rc = process_session_interrupt(runtime->manager, session_id);
	else if (strcmp(action, "kill") == 0)
		rc = process_session_kill(runtime->manager, session_id);
	else if (strcmp(action, "poll") == 0 || strcmp(action, "wait") == 0)
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
	struct process_cursor cursor;
	const char *cursor_text = json_string(root, "cursor");
	const cJSON *wait_arg = cJSON_GetObjectItemCaseSensitive(root, "wait_ms");
	if (wait_arg && (!cJSON_IsNumber(wait_arg) || wait_arg->valuedouble < 0 ||
			 wait_arg->valuedouble > 60000 ||
			 wait_arg->valuedouble != (double)wait_arg->valueint)) {
		cJSON_Delete(root);
		(void)tool_result_error(result, "invalid_request",
					"wait_ms must be an integer from 0 to 60000");
		MORPH_RETURN(-EINVAL);
	}
	uint64_t wait_ms = json_u64(root, "wait_ms", 30000);
	int used = 0;
	if (cursor_text && (sscanf(cursor_text, "%zu:%zu%n", &cursor.stdout_offset,
				   &cursor.stderr_offset, &used) != 2 ||
			    cursor_text[used] != '\0')) {
		cJSON_Delete(root);
		MORPH_RETURN(-EINVAL);
	}
	if (!strcmp(action, "wait"))
		rc = process_session_wait(runtime->manager, session_id,
			cursor_text ? &cursor : NULL, wait_ms, process_wait_cancelled,
			(void *)result->runtime, &snapshot);
	else
		rc = process_session_poll(runtime->manager, session_id,
			cursor_text ? &cursor : NULL, !strcmp(action, "poll") ? wait_ms : 0,
			process_wait_cancelled, (void *)result->runtime, &snapshot);
	if (rc == 0) {
		rc = result_snapshot(result, session_id, NULL, NULL, &snapshot, NULL);
		if (!rc && snapshot.state >= PROCESS_EXITED)
			exec_jobs_observed(runtime->jobs, session_id);
		if (rc == 0 && result->data) {
			int empty = (!snapshot.stdout_text || !snapshot.stdout_text[0]) &&
				    (!snapshot.stderr_text || !snapshot.stderr_text[0]);
			cJSON_AddBoolToObject(result->data, "empty_poll",
					      !strcmp(action, "poll") && empty &&
						  snapshot.state < PROCESS_EXITED);
			cJSON_AddNumberToObject(result->data, "wait_ms", (double)wait_ms);
		}
	}
	process_snapshot_cleanup(&snapshot);
	cJSON_Delete(root);
	return rc;
}

static int exec_background_list(void *opaque, int64_t owner, cJSON **out)
{
	struct exec_runtime *runtime = opaque;
	return exec_jobs_list(runtime->jobs, owner, out);
}

static int exec_background_wait(void *opaque, int64_t owner, const char *turn_id,
	int (*cancelled)(void *), void *cancel_data, cJSON **out)
{
	struct exec_runtime *runtime = opaque;
	return exec_jobs_wait(runtime->jobs, owner, turn_id, cancelled, cancel_data, out);
}

static void exec_environment(void *opaque, struct prompt_context_input *out)
{
	struct exec_runtime *runtime = opaque;

	out->cwd = tool_context_workdir(runtime->tool_context);
	out->shell = process_manager_shell(runtime->manager);
	/* Per-command capability policies cannot be expressed as a single mode. */
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
	rc = tool_path_policy_init(tctx);
	if (rc != 0)
		return rc;
	rc = exec_runtime_create(tctx, config, &runtime);
	if (rc != 0)
		return rc;
	memset(&exec_spec, 0, sizeof(exec_spec));
	exec_spec.origin = TOOL_ORIGIN_BUILTIN;
	exec_spec.name = "exec";
	exec_spec.title = "Execute command";
	exec_spec.description =
	    runtime->allow_host_execution
		? "Run a shell command with command approval, timeout, and process sessions. "
		  "Commands are sandboxed by default. Explicit host=true runs without the child "
		  "OS sandbox, with the user's file and network access. Use only when the task "
		  "requires host automation, such as macOS System Events AppleScript. "
		  "macOS privacy consent still applies. Do not silently retry in host mode. "
		  "Background sessions have no default timeout; set timeout_ms to limit them."
		: "Run a shell command with workdir, policy, timeout, yield, and process sessions. "
		  "Host execution is disabled. Background sessions have no default timeout; "
		  "set timeout_ms to limit them. For error.code=sandbox_denied, request the "
		  "smallest "
		  "additional capability and retry the exact command.";
	exec_spec.input_schema =
		"{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},"
		"\"description\":{\"type\":\"string\",\"description\":"
		"\"Short action summary in the user response language, e.g. Check HTTP timeout settings.\"},"
		"\"workdir\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\","
		"\"minimum\":0},\"yield_time_ms\":{\"type\":\"integer\",\"minimum\":0},"
		"\"pty\":{\"type\":\"boolean\"},\"host\":{\"type\":\"boolean\","
		"\"description\":\"Explicit host execution; requires exec.allow_host_execution. Default false.\"},"
		"\"notify_only\":{\"type\":\"boolean\",\"description\":\"Do not wait before ending the turn. Use only when the user explicitly asks to start a task and be notified later. Default false.\"},"
		"\"service\":{\"type\":\"boolean\",\"description\":\"Persistent server or watcher; do not wait for exit before replying. Default false.\"},"
		"\"background\":{\"type\":\"boolean\"},\"cmd\":{\"type\":\"string\"}},"
		"\"required\":[\"command\"],\"additionalProperties\":false}";
	exec_spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	exec_spec.exec = exec_run;
	exec_spec.get_environment = exec_environment;
	exec_spec.background_list = exec_background_list;
	exec_spec.background_wait = exec_background_wait;
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
	process_spec.description =
	    "List this session processes, wait for exit, poll output, write to, interrupt, or kill. "
	    "Use action=wait for builds/tests: it waits for exit despite intermediate output. "
	    "Use poll for interactive output. Both default to wait_ms=30000 (0-60000); "
	    "wait_ms=0 reads immediately. Pass next_cursor as cursor for incremental output. "
	    "Never start shell sleep or another delay tool just to wait for a process. "
	    "Explicit cursors are replayable; output files contain redacted retained windows "
	    "and are removed at runtime shutdown. Cancelling a wait does not stop the process.";
	process_spec.input_schema =
	    "{\"type\":\"object\",\"properties\":{\"session_id\":{\"type\":\"string\"},\"action\":{"
	    "\"type\":\"string\",\"enum\":[\"list\",\"poll\",\"wait\",\"write\",\"interrupt\",\"kill\"]},\"input\":{"
	    "\"type\":\"string\"},\"wait_ms\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":"
	    "60000},\"cursor\":{\"type\":\"string\"}},\"required\":[\"action\"],"
	    "\"additionalProperties\":false}";
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

static struct exec_runtime *exec_runtime_lookup(struct tool_registry *reg)
{
	struct tool_entry *entry = reg ? tool_lookup(reg, "exec") : NULL;
	return entry && entry->exec == exec_run ? entry->user_data : NULL;
}

int exec_tool_set_event_callback(struct tool_registry *reg, morph_event_cb cb,
				 void *user_data)
{
	struct exec_runtime *runtime = exec_runtime_lookup(reg);
	if (!runtime)
		MORPH_RETURN(-ENOENT);
	exec_jobs_bind(runtime->jobs, runtime->manager, cb, user_data);
	return 0;
}

int exec_tool_jobs_list(struct tool_registry *reg, int64_t owner, cJSON **out)
{
	struct exec_runtime *runtime = exec_runtime_lookup(reg);
	if (runtime)
		return exec_jobs_list(runtime->jobs, owner, out);
	*out = cJSON_CreateArray();
	MORPH_RETURN(*out ? 0 : -ENOMEM);
}

int exec_tool_job_read(struct tool_registry *reg, int64_t owner, const char *id,
			cJSON **out)
{
	struct exec_runtime *runtime = exec_runtime_lookup(reg);
	if (!runtime)
		MORPH_RETURN(-ENOENT);
	return exec_jobs_read(runtime->jobs, owner, id, out);
}

int exec_tool_job_stop(struct tool_registry *reg, int64_t owner, const char *id)
{
	struct exec_runtime *runtime = exec_runtime_lookup(reg);
	if (!runtime) MORPH_RETURN(-ENOENT);
	return exec_jobs_stop(runtime->jobs, owner, id);
}

struct exec_jobs *exec_tool_jobs_handle(struct tool_registry *reg)
{
	struct exec_runtime *runtime = exec_runtime_lookup(reg);
	return runtime ? runtime->jobs : NULL;
}
