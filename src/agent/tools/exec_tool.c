#include "exec_tool.h"
#include "agent/agent_control.h"
#include "exec_jobs.h"
#include "agent/tool_runtime.h"
#include "http/client.h"
#include "agent/environment_context.h"
#include "agent/tool_context.h"
#include "config/config.h"
#include "exec/process.h"
#include "sandbox.h"
#include "sandbox/tool_paths.h"
#include "sandbox/permission.h"
#include "path_policy.h"
#include "permission/command_analysis.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "cJSON.h"
#include "nb/random.h"
#include "cJSON.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include "nb/thread.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXEC_DEFAULT_TIMEOUT_MS 120000u
#define EXEC_DEFAULT_YIELD_MS 10000u

struct exec_runtime {
	nb_mutex *mutex;
	int refs;
	struct process_manager *manager;
	struct exec_jobs *jobs;
	struct tool_context *tool_context;
	uint64_t default_timeout_ms;
	uint64_t default_yield_time_ms;
	uint64_t max_wait_ms;
	char scratch[PATH_MAX];
};

#define EXEC_SCRATCH_MAX_DEPTH 64

static void exec_scratch_directory_cleanup(DIR **directory)
{
	if (directory && *directory)
		closedir(*directory);
}

/* Only traverse owned scratch through directory descriptors. Symlinks are
 * unlinked as leaves; external paths are never followed during cleanup. */
static void exec_scratch_remove_contents(int descriptor, int depth)
{
	DIR *directory NB_CLEANUP(exec_scratch_directory_cleanup) = fdopendir(descriptor);
	if (!directory) {
		close(descriptor);
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(directory))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		struct stat info = {0};
		if (fstatat(dirfd(directory), entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0)
			continue;
		if (S_ISDIR(info.st_mode)) {
			if (depth >= EXEC_SCRATCH_MAX_DEPTH)
				continue;
			int child = openat(dirfd(directory), entry->d_name,
				O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
			if (child >= 0) {
				exec_scratch_remove_contents(child, depth + 1);
				(void)unlinkat(dirfd(directory), entry->d_name, AT_REMOVEDIR);
			}
		} else {
			(void)unlinkat(dirfd(directory), entry->d_name, 0);
		}
	}
}

static void exec_scratch_remove(const char *path)
{
	if (!path || !*path)
		return;
	int descriptor = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (descriptor < 0)
		return;
	exec_scratch_remove_contents(descriptor, 0);
	(void)rmdir(path);
}

static void exec_runtime_destroy(void *opaque)
{
	struct exec_runtime *runtime = opaque;
	int destroy = 0;

	if (!runtime)
		return;
	nb_mutex_lock(runtime->mutex);
	runtime->refs--;
	if (runtime->refs == 0)
		destroy = 1;
	nb_mutex_unlock(runtime->mutex);
	if (!destroy)
		return;
	process_manager_destroy(runtime->manager);
	exec_jobs_destroy(runtime->jobs);
	(nb_mutex_free(runtime->mutex), runtime->mutex = NULL);
	exec_scratch_remove(runtime->scratch);
	free(runtime);
}

static void exec_runtime_discard(struct exec_runtime *runtime)
{
	if (!runtime)
		return;
	exec_scratch_remove(runtime->scratch);
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
		return NB_ERR_INVAL;
	*out = NULL;
	runtime = calloc(1, sizeof(*runtime));
	if (!runtime)
		return NB_ERR_NOMEM;
	nb_buf directory NB_CLEANUP(nb_buf_cleanup) = {0};
	rc = nb_file_temp_dir(&directory, "/tmp", "morph-exec-", NULL);
	if (rc == NB_OK)
		rc = nb_str_copy_to(nb_buf_str(&directory), runtime->scratch,
				    sizeof(runtime->scratch));
	if (rc != NB_OK) {
		if (!nb_buf_empty(&directory))
			(void)nb_file_remove_all(nb_buf_cstr(&directory), 0, NULL);
		free(runtime);
		return rc == NB_ERR_RANGE ? NB_ERR_NAME_TOO_LONG : rc;
	}

	runtime->refs = 2;
	runtime->tool_context = tctx;
	runtime->default_timeout_ms = exec_config ?
		(uint64_t)exec_config->default_timeout_ms : EXEC_DEFAULT_TIMEOUT_MS;
	runtime->default_yield_time_ms = exec_config ?
		(uint64_t)exec_config->yield_time_ms : EXEC_DEFAULT_YIELD_MS;
	runtime->max_wait_ms = exec_config && exec_config->max_wait_ms > 0 ?
		(uint64_t)exec_config->max_wait_ms : EXEC_MAX_WAIT_MS_DEFAULT;
	rc = ((runtime->mutex = nb_mutex_new(0)) ? NB_OK : NB_ERR_NOMEM);
	if (rc != 0) {
		exec_runtime_discard(runtime);
		return rc;
	}
	rc = exec_jobs_create(&runtime->jobs);
	if (rc != 0) {
		(nb_mutex_free(runtime->mutex), runtime->mutex = NULL);
		exec_runtime_discard(runtime);
		return rc;
	}
	config.completed = exec_jobs_complete;
	config.completed_user_data = runtime->jobs;
	rc = process_manager_create(&config, &runtime->manager);
	if (rc != 0) {
		exec_jobs_destroy(runtime->jobs);
		(nb_mutex_free(runtime->mutex), runtime->mutex = NULL);
		exec_runtime_discard(runtime);
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
			int host, int network, struct tool_result *result)
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
		(void)tool_result_error_copy(result, "approval_required",
					     "shell command could not be analyzed safely");
		return NB_ERR_NOT_PERMITTED;
	}
	if (runtime->tool_context) {
		if (analysis.commands.len >= (size_t)INT_MAX) {
			bash_parse_result_cleanup(&analysis);
			return NB_ERR_TOO_BIG;
		}
		programs = calloc(analysis.commands.len + 1, sizeof(*programs));
		if (!programs) {
			bash_parse_result_cleanup(&analysis);
			return NB_ERR_NOMEM;
		}
		for (size_t i = 0; i < analysis.commands.len; i++) {
			const struct bash_parse_command *parsed = nb_vec_at(&analysis.commands, i);
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
			struct permission_review review = {0};
			struct tool_operation operation = {
				.review_out = &review,
				.kind = TOOL_OP_COMMAND,
				.host = host, .network = network,
				.require_user = result->runtime ?
					result->runtime->permission_require_user : 0,
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
				const char *code = rc == NB_ERR_PERMISSION ? "permission_denied"
						   : rc == NB_ERR_NOT_PERMITTED
							   ? "approval_unavailable"
							   : "approval_required";
				const char *message =
					rc == NB_ERR_PERMISSION
						? review.reason[0]
							  ? review.reason
							  : "Command permission was denied."
					: rc == NB_ERR_NOT_PERMITTED
						? "Interactive command approval is unavailable."
						: "Command approval failed.";

				bash_parse_result_cleanup(&analysis);
				(void)tool_result_error_format(
					result, code,
					"%s Do not bypass a denial through another tool or route.",
					message);
				return rc != 0 ? rc : NB_ERR_NOT_PERMITTED;
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
	cJSON_AddNumberToObject(object, "stdout_total_bytes", (double)snapshot->stdout_total_bytes);
	cJSON_AddNumberToObject(object, "stderr_total_bytes", (double)snapshot->stderr_total_bytes);
	cJSON_AddNumberToObject(object, "stdout_path_bytes", (double)snapshot->stdout_path_bytes);
	cJSON_AddNumberToObject(object, "stderr_path_bytes", (double)snapshot->stderr_path_bytes);
	cJSON_AddNumberToObject(object, "stdout_retained_from",
		(double)snapshot->stdout_retained_from);
	cJSON_AddNumberToObject(object, "stderr_retained_from",
		(double)snapshot->stderr_retained_from);
	cJSON_AddNumberToObject(object, "stdout_unrecoverable_bytes",
		(double)snapshot->stdout_unrecoverable_bytes);
	cJSON_AddNumberToObject(object, "stderr_unrecoverable_bytes",
		(double)snapshot->stderr_unrecoverable_bytes);
	cJSON_AddBoolToObject(object, "stdout_path_truncated", snapshot->stdout_path_truncated);
	cJSON_AddBoolToObject(object, "stderr_path_truncated", snapshot->stderr_path_truncated);
	if (snapshot->stdout_capture_error)
		cJSON_AddStringToObject(object, "stdout_capture_error",
					nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
						   snapshot->stdout_capture_error));
	if (snapshot->stderr_capture_error)
		cJSON_AddStringToObject(object, "stderr_capture_error",
					nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
						   snapshot->stderr_capture_error));
	if (snapshot->stdout_truncated || snapshot->stderr_truncated ||
	    snapshot->stdout_path_truncated || snapshot->stderr_path_truncated)
		cJSON_AddStringToObject(object, "output_note",
			"Inline output may omit bytes. Read stdout_path/stderr_path for redacted "
			"prefix [0, *_path_bytes); cursor and file offsets use the same bytes. "
			"next_cursor reads only later output. If *_path_truncated is true, the "
			"file stopped at its byte count because of quota or capture failure. "
			"The range [*_path_bytes, *_retained_from) is unavailable when "
			"*_unrecoverable_bytes is nonzero; rerun a narrower query to recover it.");
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
	cJSON *data NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	int rc;

	if (!data)
		return NB_ERR_NOMEM;
	cJSON_AddStringToObject(data, "session_id", session_id);
	if (command)
		cJSON_AddStringToObject(data, "command", command);
	if (workdir)
		cJSON_AddStringToObject(data, "workdir", workdir);
	if (execution_mode)
		cJSON_AddStringToObject(data, "execution_mode", execution_mode);
	snapshot_add_json(data, snapshot);
	if (result->runtime && result->runtime->agent_id &&
	    strcmp(result->runtime->agent_id, "/root") && snapshot->stderr_text &&
	    strstr(snapshot->stderr_text, "temp file for here document"))
		cJSON_AddStringToObject(data, "next_action",
			"This shell creates here-document temporary files outside the private "
			"scratch directory even when TMPDIR is set. Write the file with printf "
			"or python under $MORPH_AGENT_SCRATCH, or use a configured shell that "
			"honors TMPDIR. Keep workspace and global /tmp writes restricted.");
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
	rc = tool_result_success_take(result, &data);
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
	nb_buf canonical NB_CLEANUP(nb_buf_destroy) = {0};
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
		if (nb_path_canonical(&canonical, candidate, NULL) != NB_OK ||
		    nb_str_copy_to(nb_buf_str(&canonical), resolved, sizeof(resolved)) != NB_OK)
			return;
	} else if (snprintf(git_path, sizeof(git_path), "%s/%s", workdir,
			    candidate) <= 0 || nb_path_canonical(&canonical, git_path,
				NULL) != NB_OK ||
		   nb_str_copy_to(nb_buf_str(&canonical), resolved, sizeof(resolved)) != NB_OK) {
		return;
	}
	if (snprintf(git_path, sizeof(git_path), "%s/commondir",
		     resolved) > 0) {
		file = fopen(git_path, "r");
		if (file) {
			char common[PATH_MAX];
			char common_path[PATH_MAX];
			nb_buf common_resolved NB_CLEANUP(nb_buf_destroy) = {0};
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
				    nb_path_canonical(&common_resolved, common_path, NULL) == NB_OK)
					snprintf(resolved, sizeof(resolved),
						 "%s", nb_buf_cstr(&common_resolved));
			}
			fclose(file);
		}
	}
	snprintf(out, out_size, "%s", resolved);
}

static int process_wait_cancelled(void *opaque)
{
	const struct tool_runtime_context *runtime = opaque;

	return http_wait_cancelable(runtime ? &runtime->http : NULL, 0) < 0;
}

static int exec_run(const char *args_json, struct tool_result *result,
			void *user_data)
{
	struct exec_runtime *runtime = user_data;
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	const char *command;
	const char *workdir;
	struct process_spawn_options options;
	struct sandbox_config sandbox NB_CLEANUP(sandbox_config_cleanup) = {0};
	struct agent_shell_scope agent_scope NB_CLEANUP(agent_shell_scope_cleanup) = {0};
	char resolved_workdir[PATH_MAX];
	char git_dir[PATH_MAX] = {0};
	char principal[TOOL_CONTEXT_CLI_NAME_MAX] = "shell";
	char *read_paths[12];
	char *write_paths[4];
	char *delete_paths[4];
	int read_path_count = 0;
	int write_path_count = 0;
	int delete_path_count = 0;
	struct process_snapshot snapshot NB_CLEANUP(process_snapshot_cleanup) = {0};
	char session_id[PROCESS_SESSION_ID_MAX] = {0};
	int host;
	int network;
	int readonly;
	int scoped_readonly;
	int rc;
	struct permission_policy policy = permission_policy_default();

	if (!runtime || !result)
		return NB_ERR_INVAL;
	root = args_json ? cJSON_Parse(args_json) : NULL;
	if (!root) {
		(void)tool_result_error_copy(result, "invalid_request",
					     "exec arguments must be a JSON object");
		return NB_ERR_INVAL;
	}
	command = json_string(root, "command");
	workdir = json_string(root, "workdir");
	if (!workdir)
		workdir = json_string(root, "cwd");
	if (!command || !*command) {

		(void)tool_result_error_copy(result, "invalid_request", "command is required");
		return NB_ERR_INVAL;
	}
	const struct tool_runtime_context *actor = result->runtime;
	int scoped_child = actor && actor->agents && actor->agent_id && *actor->agent_id &&
		strcmp(actor->agent_id, "/root");
	if (scoped_child &&
	    (json_bool(root, "service", 0) || json_bool(root, "notify_only", 0))) {
		(void)tool_result_error_copy(result, "persistent_child_job_disabled",
			"Child jobs must finish before their write_paths are released. "
			"Ask the primary agent to start persistent services or notify-only jobs.");
		return NB_ERR_PERMISSION;
	}
	if (runtime->tool_context)
		(void)tool_context_get_permission_policy(runtime->tool_context, &policy);
	scoped_readonly = result->runtime && agent_control_shell_readonly(
		result->runtime->agents, result->runtime->agent_id);
	readonly = scoped_readonly || (runtime->tool_context && policy.access == PERMISSION_READ_ONLY);
	if (json_bool(root, "host", 0) && !runtime->tool_context) {

		(void)tool_result_error_copy(
			result, "host_execution_disabled",
			"Host execution requires an explicit permission context.");
		return NB_ERR_PERMISSION;
	}
	host = json_bool(root, "host", 0) ||
	       (runtime->tool_context && policy.access == PERMISSION_FULL);
	network = json_bool(root, "network", 0);
	if (!workdir && runtime->tool_context)
		workdir = tool_context_workdir_borrow(runtime->tool_context);
	if (runtime->tool_context)
		(void)tool_context_command_principal(command, principal,
						     sizeof(principal));
	rc = policy_check(runtime, command, workdir, args_json, host, network, result);
	if (rc != 0) {

		return rc;
	}
	memset(&options, 0, sizeof(options));
	options.command = command;
	options.workdir = workdir;
	options.background = json_bool(root, "background", json_bool(root, "service", 0));
	options.host_execution = host;
	options.kill_descendants_on_exit = scoped_child;
	options.timeout_ms = json_u64(root, "timeout_ms",
		options.background ? 0 : runtime->default_timeout_ms);
	options.yield_time_ms = json_u64(root, "yield_time_ms",
		runtime->default_yield_time_ms);
	options.pty = json_bool(root, "pty", 0);
	memset(&sandbox, 0, sizeof(sandbox));
	const char *workspace = runtime->tool_context
					? tool_context_workdir_borrow(runtime->tool_context)
					: workdir;
	nb_buf canonical_workspace NB_CLEANUP(nb_buf_destroy) = {0};
	if (workspace && nb_path_canonical(&canonical_workspace, workspace, NULL) == NB_OK &&
	    nb_str_copy_to(nb_buf_str(&canonical_workspace), resolved_workdir,
			   sizeof(resolved_workdir)) == NB_OK) {
		read_paths[read_path_count++] = resolved_workdir;
		if (!readonly) {
			write_paths[write_path_count++] = resolved_workdir;
			delete_paths[delete_path_count++] = resolved_workdir;
		}
	}
	read_paths[read_path_count++] = "/usr";
	read_paths[read_path_count++] = "/bin";
	read_paths[read_path_count++] = "/sbin";
	read_paths[read_path_count++] = "/System";
	read_paths[read_path_count++] = "/Library";
	read_paths[read_path_count++] = "/opt/homebrew";
	read_paths[read_path_count++] = "/private";
	read_paths[read_path_count++] = "/tmp";
	write_paths[write_path_count++] = readonly ? runtime->scratch : "/tmp";
	delete_paths[delete_path_count++] = readonly ? runtime->scratch : "/tmp";
	exec_resolve_worktree_git_dir(workspace, git_dir, sizeof(git_dir));
	if (git_dir[0] && !readonly) {
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
	sandbox.network_access = network || host;
	sandbox.process_exec = 1;
	sandbox.allow_pty = options.pty;
	sandbox.allow_process_info = 1;
	sandbox.allow_temp = !readonly;
	sandbox.allow_ipc = 1;
	sandbox.development_runtime = 1;
	rc = 0;
	if (rc == 0 && runtime->tool_context && git_dir[0] && !readonly) {
		rc = tool_context_add_builtin_path(runtime->tool_context, TOOL_PATH_WRITE, git_dir);
		if (rc == 0)
			rc = tool_context_add_builtin_path(runtime->tool_context, TOOL_PATH_DELETE, git_dir);
	}
	if (rc == 0 && runtime->tool_context)
		rc = sandbox_permission_prepare(runtime->tool_context, principal, runtime->scratch,
						&sandbox);
	else if (rc == 0)
		rc = sandbox_prepare_tool_paths(&sandbox);
	if (rc == 0 && scoped_readonly) {
		rc = agent_control_shell_scope_copy(result->runtime->agents,
			result->runtime->agent_id, runtime->scratch, &agent_scope);
		if (rc == 0) {
			struct sandbox_config ceiling = {
				.read_all = 1, .network_access = 1,
				.write_paths = agent_scope.write_paths,
				.write_paths_count = agent_scope.write_path_count,
				.delete_paths = agent_scope.write_paths,
				.delete_paths_count = agent_scope.write_path_count,
			};
			rc = sandbox_permission_intersect(&sandbox, &ceiling);
		}
	}
	sandbox.network_access |= network || host;
	sandbox.allow_pty = options.pty;
	if (rc != 0) {

		(void)tool_result_error_copy(
			result, "sandbox_policy_failed",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	sandbox.logger = tool_context_logger_borrow(runtime->tool_context);
	options.logger = sandbox.logger;
	options.sandbox = host ? NULL : &sandbox;
	options.temp_dir = agent_scope.scratch;
	options.cancelled = process_wait_cancelled;
	options.cancel_data = (void *)result->runtime;
	memset(&snapshot, 0, sizeof(snapshot));
	rc = process_spawn(runtime->manager, &options, session_id, &snapshot);
	if (rc != 0) {
		if (rc == NB_ERR_CANCELLED && session_id[0])
			(void)process_session_kill(runtime->manager, session_id);
		const char *code = rc == NB_ERR_NOT_FOUND ? "invalid_workdir"
				   : rc == NB_ERR_NOT_DIR ? "invalid_workdir"
							  : "spawn_failed";

		(void)tool_result_error_copy(
			result, code,
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		process_snapshot_cleanup(&snapshot);
		return rc;
	}
	if (snapshot.state < PROCESS_EXITED) {
		const struct tool_runtime_context *context = result->runtime;
		rc = exec_jobs_add(runtime->jobs, session_id, command,
			json_string(root, "description"), workdir,
			context ? context->memory_session_id : 0,
			context ? context->turn_id : NULL, json_bool(root, "service", 0),
			json_bool(root, "notify_only", 0), host, sandbox.network_access);
		if (rc != 0) {
			(void)process_session_kill(runtime->manager, session_id);
			process_snapshot_cleanup(&snapshot);

			return rc;
		}
	}
	/* A cancelled worker may register after its turn has already finished. */
	if (process_wait_cancelled((void *)result->runtime)) {
		(void)process_session_kill(runtime->manager, session_id);
		process_snapshot_cleanup(&snapshot);

		return NB_ERR_CANCELLED;
	}
	rc = result_snapshot(result, session_id, command, workdir, &snapshot,
		host ? "host" : "sandbox");
	process_snapshot_cleanup(&snapshot);

	return rc;
}

static int background_run(const char *args_json, struct tool_result *result,
			   void *user_data)
{
	struct exec_runtime *runtime = user_data;
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	const char *session_id;
	const char *action;
	const char *input;
	struct process_snapshot snapshot NB_CLEANUP(process_snapshot_cleanup) = {0};
	int rc;

	if (!runtime || !result)
		return NB_ERR_INVAL;
	root = args_json ? cJSON_Parse(args_json) : NULL;
	if (!root) {
		(void)tool_result_error_copy(result, "invalid_request",
					     "background arguments must be a JSON object");
		return NB_ERR_INVAL;
	}
	session_id = json_string(root, "session_id");
	action = json_string(root, "action");
	input = json_string(root, "input");
	if (action && !strcmp(action, "list")) {
		cJSON *jobs NB_CLEANUP(cJSON_DeleteOwner) = NULL;
		int64_t owner = result->runtime ? result->runtime->memory_session_id : 0;
		rc = exec_jobs_list(runtime->jobs, owner, &jobs);

		if (rc) return rc;
		cJSON *data NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		if (!data || !cJSON_AddItemToObject(data, "jobs", jobs))
			return NB_ERR_NOMEM;
		jobs = NULL;
		rc = tool_result_success_take(result, &data);
		return rc;
	}
	if (!session_id || !action) {

		(void)tool_result_error_copy(result, "invalid_request",
					     "session_id and action are required");
		return NB_ERR_INVAL;
	}
	rc = exec_jobs_authorize(runtime->jobs, session_id,
		result->runtime ? result->runtime->memory_session_id : 0);
	if (rc) {

		(void)tool_result_error_copy(result, "process_not_found",
					     "Process not found in this session.");
		return rc;
	}
	memset(&snapshot, 0, sizeof(snapshot));
	struct process_cursor cursor;
	const char *cursor_text = json_string(root, "cursor");
	const cJSON *wait_arg = cJSON_GetObjectItemCaseSensitive(root, "wait_ms");
	uint64_t default_wait_ms = runtime->max_wait_ms < 30000 ? runtime->max_wait_ms : 30000;
	double requested_wait_ms = wait_arg ? wait_arg->valuedouble : (double)default_wait_ms;
	if (wait_arg && (!cJSON_IsNumber(wait_arg) || !isfinite(requested_wait_ms) ||
	    requested_wait_ms < 0 || requested_wait_ms > (double)runtime->max_wait_ms ||
	    floor(requested_wait_ms) != requested_wait_ms)) {

		(void)tool_result_error_format(
			result, "invalid_request",
			"wait_ms must be an integer from 0 to %llu; omit it to wait up to %llu ms",
			(unsigned long long)runtime->max_wait_ms,
			(unsigned long long)default_wait_ms);
		return NB_ERR_INVAL;
	}
	int collects = !strcmp(action, "wait") || !strcmp(action, "poll");
	uint64_t wait_ms = collects ? (uint64_t)requested_wait_ms : 0;
	int used = 0;
	if (cursor_text && (sscanf(cursor_text, "%zu:%zu%n", &cursor.stdout_offset,
				   &cursor.stderr_offset, &used) != 2 ||
			    cursor_text[used] != '\0')) {

		return NB_ERR_INVAL;
	}
	if (!strcmp(action, "write") && input && *input && runtime->tool_context) {
		int host = 0, network = 0;

		rc = exec_jobs_authority(runtime->jobs, session_id, &host, &network);
		if (rc == 0) {
			struct tool_operation operation = {
				.kind = TOOL_OP_TOOL_CALL, .tool_name = "background",
				.principal = session_id, .action = input, .target = session_id,
				.scope = "Input to an existing process",
				.reason = "The process retains its original access, "
					"including earlier file grants.",
				.details_json = args_json,
				.host = host, .network = network,
			};
			rc = tool_context_check_operation(runtime->tool_context, &operation);
		}
		if (rc != 0) {

			(void)tool_result_error_copy(
				result,
				rc == NB_ERR_NOT_PERMITTED ? "approval_unavailable"
							   : "permission_denied",
				"Process input requires permission for its retained access.");
			return rc;
		}
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
		rc = NB_ERR_INVAL;
	if (rc != 0) {
		const char *code = rc == NB_ERR_NOT_FOUND     ? "process_not_found"
				   : rc == NB_ERR_BROKEN_PIPE ? "process_not_running"
				   : rc == NB_ERR_AGAIN	      ? "io_error"
							      : "invalid_request";

		(void)tool_result_error_copy(
			result, code,
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	if (!strcmp(action, "wait") && wait_ms)
		exec_jobs_wait_event(runtime->jobs, session_id, 1);
	if (!strcmp(action, "wait"))
		rc = process_session_wait(runtime->manager, session_id,
			cursor_text ? &cursor : NULL, wait_ms, process_wait_cancelled,
			(void *)result->runtime, &snapshot);
	else
		rc = process_session_poll(runtime->manager, session_id,
			cursor_text ? &cursor : NULL, !strcmp(action, "poll") ? wait_ms : 0,
			process_wait_cancelled, (void *)result->runtime, &snapshot);
	if (!strcmp(action, "wait") && wait_ms)
		exec_jobs_wait_event(runtime->jobs, session_id, 0);
	if (rc == 0) {
		rc = result_snapshot(result, session_id, NULL, NULL, &snapshot, NULL);
		if (!rc && snapshot.state >= PROCESS_EXITED)
			exec_jobs_observed(runtime->jobs, session_id);
		cJSON *data = cJSON_GetObjectItemCaseSensitive(result->envelope, "data");
		if (rc == 0 && cJSON_IsObject(data)) {
			int empty = (!snapshot.stdout_text || !snapshot.stdout_text[0]) &&
				    (!snapshot.stderr_text || !snapshot.stderr_text[0]);
			cJSON_AddBoolToObject(data, "empty_poll",
					      !strcmp(action, "poll") && empty &&
						  snapshot.state < PROCESS_EXITED);
			cJSON_AddNumberToObject(data, "wait_ms", (double)wait_ms);
			cJSON_AddNumberToObject(data, "max_wait_ms", (double)runtime->max_wait_ms);
			if (!strcmp(action, "wait") && snapshot.state < PROCESS_EXITED)
				cJSON_AddStringToObject(data, "next_action",
					"The process is still running. Wait again with action=wait "
					"and the same session_id; wait_ms does not set "
					"the process timeout.");
		}
	}
	process_snapshot_cleanup(&snapshot);

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

static void exec_background_cancel(void *opaque, int64_t owner, const char *turn_id)
{
	struct exec_runtime *runtime = opaque;
	exec_jobs_cancel_turn(runtime->jobs, owner, turn_id);
}

static void exec_environment(void *opaque, struct prompt_context_input *out)
{
	struct exec_runtime *runtime = opaque;
	struct permission_policy policy;

	out->cwd = tool_context_workdir_borrow(runtime->tool_context);
	out->shell = process_manager_shell_borrow(runtime->manager);
	if (tool_context_get_permission_policy(runtime->tool_context, &policy) == 0)
		out->sandbox = permission_access_name(policy.access);
}

int exec_tool_init(struct tool_registry *reg, struct tool_context *tctx,
		   const struct config_exec *config)
{
	struct exec_runtime *runtime;
	struct tool_spec exec_spec NB_CLEANUP(tool_spec_cleanup) = {0};
	struct tool_spec background_spec NB_CLEANUP(tool_spec_cleanup) = {0};
	int rc;

	if (!reg)
		return NB_ERR_INVAL;
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
	    runtime->tool_context
		? "Run a shell command with command approval, timeout, and background jobs. "
		  "Commands are sandboxed by default. Explicit host=true runs without the child "
		  "OS sandbox, with the user's file and network access. Use only when the task "
		  "requires host automation, such as macOS System Events AppleScript. "
		  "Child agents use the same host and network permission policy as the parent. "
		  "Use network=true for commands needing sockets when network access is not "
		  "already available. Full Access uses host execution by default. "
		  "macOS privacy consent still applies. Do not silently retry in host mode. "
		  "For error.code=sandbox_denied, request the smallest additional capability "
		  "and retry the exact command. Background sessions have no default timeout; "
		  "set timeout_ms to limit them. Use background to wait, inspect, or stop "
		  "a running job."
		: "Run a shell command with workdir, policy, timeout, yield, and background jobs. "
		  "Host execution is disabled. Background sessions have no default timeout; "
		  "set timeout_ms to limit them. For error.code=sandbox_denied, request the "
		  "smallest "
		  "additional capability and retry the exact command. Use background to manage "
		  "running jobs.";
	exec_spec.input_schema =
		"{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\","
		"\"description\":\"Reuse discovered paths, indexes, and extracted files. "
		"Batch independent reads needed for the next decision. Prefer targeted searches "
		"and relevant sections over dumping trees or documents. When available, prefer "
		"rg for content searches and fd for file-name or path discovery. If fd is "
		"unavailable, use rg --files when rg is available. Fall back to grep/find "
		"when these tools are unavailable. When availability is unknown, check once "
		"in the execution shell and reuse the result instead of probing repeatedly. "
		"After truncation, read relevant ranges from stdout_path/stderr_path; these "
		"redacted files start at byte zero and share a 64 MiB quota. Check "
		"*_path_bytes and *_path_truncated before assuming a file is complete. "
		"Cursors use redacted stream byte offsets; next_cursor reads later output, "
		"not omitted inline bytes. *_unrecoverable_bytes marks missing data between "
		"the file prefix and memory window; narrow the query to recover it. "
		"Do not repeat the same oversized command.\"},"
		"\"description\":{\"type\":\"string\",\"description\":"
		"\"Short action summary in the user response language, e.g. Check HTTP timeout settings.\"},"
		"\"workdir\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\","
		"\"minimum\":0},\"yield_time_ms\":{\"type\":\"integer\",\"minimum\":0},"
		"\"network\":{\"type\":\"boolean\"},\"pty\":{\"type\":\"boolean\"},\"host\":{\"type\":\"boolean\","
		"\"description\":\"Host execution under the shared permission policy. "
		"Full Access enables it by default, including for child agents.\"},"
		"\"notify_only\":{\"type\":\"boolean\",\"description\":\"Do not wait before ending the turn. Use only when the user explicitly asks to start a task and be notified later. Default false.\"},"
		"\"service\":{\"type\":\"boolean\",\"description\":\"Persistent server or watcher; do not wait for exit before replying. Default false.\"},"
		"\"background\":{\"type\":\"boolean\"}},"
		"\"required\":[\"command\"],\"additionalProperties\":false}";
	exec_spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	exec_spec.exec = exec_run;
	exec_spec.get_environment = exec_environment;
	exec_spec.background_list = exec_background_list;
	exec_spec.background_wait = exec_background_wait;
	exec_spec.background_cancel = exec_background_cancel;
	exec_spec.user_data = runtime;
	exec_spec.user_data_destroy = exec_runtime_destroy;
	background_spec.user_data = runtime;
	background_spec.user_data_destroy = exec_runtime_destroy;
	exec_spec.flags = TOOL_FLAG_INTERNAL_APPROVAL;
	rc = tool_register_take(reg, &exec_spec);
	if (rc != 0)
		return rc;
	background_spec.origin = TOOL_ORIGIN_BUILTIN;
	background_spec.name = "background";
	background_spec.title = "Manage background jobs";
	nb_buf description NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf schema NB_CLEANUP(nb_buf_cleanup) = {0};
	rc = (nb_buf_init(&description), nb_buf_reserve(&description, 1024) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_init(&schema), nb_buf_reserve(&schema, 1024) ? 0 : NB_ERR_NOMEM);
	uint64_t default_wait_ms = runtime->max_wait_ms < 30000 ? runtime->max_wait_ms : 30000;
	if (rc == 0)
		rc = (nb_buf_printf(
			      &description,
			      "Manage background jobs started by exec in this session: list, wait "
			      "for exit, "
			      "poll output, write to, "
			      "interrupt, "
			      "or kill. Use action=wait for builds/tests: it waits for exit "
			      "despite "
			      "intermediate output. Use poll for interactive output. Choose "
			      "wait_ms "
			      "from 0 to %llu ms; omit it for %llu ms. Zero reads immediately. "
			      "This per-call wait budget is not the process timeout. "
			      "If status is running, "
			      "wait again on the same session_id. Pass next_cursor as cursor for "
			      "incremental output. Never use shell sleep just to wait for a "
			      "process. "
			      "Explicit cursors replay still-retained ranges. Output files are "
			      "redacted "
			      "prefixes starting at byte zero, sharing a 64 MiB quota and removed "
			      "at "
			      "runtime shutdown. Check *_path_bytes and *_path_truncated. "
			      "next_cursor skips inline omissions; read file ranges to recover "
			      "those. "
			      "If *_unrecoverable_bytes is nonzero, the range from *_path_bytes to "
			      "*_retained_from is lost; rerun a narrower query. "
			      "Cancelling a wait leaves the process running.",
			      (unsigned long long)runtime->max_wait_ms,
			      (unsigned long long)default_wait_ms)
			      ? 0
			      : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_printf(
			      &schema,
			      "{\"type\":\"object\",\"properties\":{\"session_id\":{\"type\":"
			      "\"string\"},"
			      "\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"poll\","
			      "\"wait\","
			      "\"write\",\"interrupt\",\"kill\"]},\"input\":{\"type\":\"string\"},"
			      "\"wait_ms\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":%llu,"
			      "\"default\":%llu,\"description\":\"Choose this per-call wait budget "
			      "in "
			      "milliseconds. Zero reads immediately. "
			      "This does not set the command timeout.\"},"
			      "\"cursor\":{\"type\":\"string\"}},\"required\":[\"action\"],"
			      "\"additionalProperties\":false}",
			      (unsigned long long)runtime->max_wait_ms,
			      (unsigned long long)default_wait_ms)
			      ? 0
			      : NB_ERR_NOMEM);
	background_spec.description = nb_buf_cstr(&description);
	background_spec.input_schema = nb_buf_cstr(&schema);
	background_spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	background_spec.exec = background_run;
	background_spec.user_data = runtime;
	background_spec.user_data_destroy = exec_runtime_destroy;
	background_spec.flags = TOOL_FLAG_INTERNAL_APPROVAL;
	background_spec.timeout_seconds = (int)((runtime->max_wait_ms + 999) / 1000) + 30;
	if (rc == 0)
		rc = tool_register_take(reg, &background_spec);
	nb_buf_cleanup(&description);
	nb_buf_cleanup(&schema);
	if (rc != 0)
		return rc;
	return 0;
}

void exec_tool_jobs_release(struct exec_tool_jobs_handle *owner)
{
	if (!owner)
		return;
	owner->jobs = NULL;
	tool_handle_release(&owner->tool);
}

int exec_tool_jobs_acquire(struct tool_registry *reg, struct exec_tool_jobs_handle *out)
{
	struct exec_tool_jobs_handle candidate NB_CLEANUP(exec_tool_jobs_release) = {0};
	int rc;

	if (!reg || !out)
		return NB_ERR_INVAL;
	if (out->tool.binding || out->jobs)
		return NB_ERR_STATE;
	rc = tool_registry_acquire(reg, "exec", &candidate.tool);
	if (rc != 0)
		return rc;
	if (candidate.tool.exec != exec_run)
		return NB_ERR_NOT_FOUND;
	struct exec_runtime *runtime = candidate.tool.user_data;
	if (!runtime)
		return NB_ERR_NOT_FOUND;
	candidate.jobs = runtime->jobs;
	*out = candidate;
	memset(&candidate, 0, sizeof(candidate));
	return 0;
}

int exec_tool_set_event_callback(struct tool_registry *reg, morph_event_cb cb, void *user_data)
{
	struct exec_tool_jobs_handle jobs NB_CLEANUP(exec_tool_jobs_release) = {0};
	int rc = exec_tool_jobs_acquire(reg, &jobs);

	if (rc != 0)
		return rc;
	struct exec_runtime *runtime = jobs.tool.user_data;
	exec_jobs_bind(jobs.jobs, runtime->manager, cb, user_data);
	return 0;
}

int exec_tool_jobs_list(struct tool_registry *reg, int64_t owner, cJSON **out)
{
	struct exec_tool_jobs_handle jobs NB_CLEANUP(exec_tool_jobs_release) = {0};
	int rc;

	if (!out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_STATE;
	rc = exec_tool_jobs_acquire(reg, &jobs);
	if (rc == NB_ERR_NOT_FOUND) {
		*out = cJSON_CreateArray();
		return *out ? 0 : NB_ERR_NOMEM;
	}
	return rc == 0 ? exec_jobs_list(jobs.jobs, owner, out) : rc;
}

int exec_tool_job_read(struct tool_registry *reg, int64_t owner, const char *id, cJSON **out)
{
	struct exec_tool_jobs_handle jobs NB_CLEANUP(exec_tool_jobs_release) = {0};
	int rc = exec_tool_jobs_acquire(reg, &jobs);

	return rc == 0 ? exec_jobs_read(jobs.jobs, owner, id, out) : rc;
}

int exec_tool_job_stop(struct tool_registry *reg, int64_t owner, const char *id)
{
	struct exec_tool_jobs_handle jobs NB_CLEANUP(exec_tool_jobs_release) = {0};
	int rc = exec_tool_jobs_acquire(reg, &jobs);

	return rc == 0 ? exec_jobs_stop(jobs.jobs, owner, id) : rc;
}
