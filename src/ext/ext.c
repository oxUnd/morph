#include "util/signals.h"
#include "sandbox/permission.h"
#include "util/child_io.h"
#include "util/cleanup.h"
#include "util/json.h"
#include "ext.h"
#include "loader.h"
#include "util/file.h"
#include "util/log.h"
#include "util/buf.h"
#include "util/error.h"
#include "manifest.h"
#include "ipc/jsonrpc.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>

int ext_load(struct morph_log *logger, struct ext *ex, const char *dir_path)
{
	if (!ex || !dir_path)
		return -EINVAL;
	if (ex->dl_handle || ex->run || ex->enabled || ex->manifest.input_schema ||
	    ex->manifest.output_schema || ex->manifest.fronts || ex->manifest.categories ||
	    ex->manifest.allowed_paths || ex->manifest.allowed_env ||
	    ex->manifest.allowed_mach_services)
		return -EEXIST;
	struct ext loaded MORPH_CLEANUP(ext_unload) = {0};
	if (strlen(dir_path) >= sizeof(loaded.path))
		return -ENAMETOOLONG;
	loaded.logger = logger;
	memcpy(loaded.path, dir_path, strlen(dir_path) + 1);
	char manifest_path[PATH_MAX];
	int rc = file_path_join(manifest_path, sizeof(manifest_path), dir_path, "manifest.toml");
	if (rc != 0)
		return rc;
	struct stat info;
	if (stat(manifest_path, &info) != 0) {
		rc = file_path_join(manifest_path, sizeof(manifest_path), dir_path,
				    "morph-ext.toml");
		if (rc != 0)
			return rc;
		if (stat(manifest_path, &info) != 0) {
			rc = morph_error_from_errno(errno);
			log_err(logger, "ext manifest not found in: %s", dir_path);
			return rc;
		}
	}
	rc = manifest_parse_file(logger, manifest_path, &loaded.manifest);
	if (rc != 0)
		return rc;
	if (strcmp(loaded.manifest.type, "exec") == 0)
		rc = ext_load_exec(&loaded, dir_path);
	else if (strcmp(loaded.manifest.type, "so") == 0)
		rc = ext_load_so(&loaded, dir_path);
	else {
		log_warn(logger, "unknown ext type: %s", loaded.manifest.type);
		rc = -EINVAL;
	}
	if (rc != 0) {
		log_err(logger, "ext_load: failed to load %s: %s", loaded.manifest.name,
			morph_strerror(rc));
		return rc;
	}
	if (!loaded.manifest.input_schema || !loaded.manifest.output_schema)
		return -EINVAL;
	loaded.enabled = 1;
	*ex = loaded;
	memset(&loaded, 0, sizeof(loaded));
	/* Embedded descriptor views must refer to the destination's inline arrays. */
	ex->tool_desc.name = ex->manifest.name;
	ex->tool_desc.title = "";
	ex->tool_desc.description = ex->manifest.description;
	ex->tool_desc.input_schema = ex->manifest.input_schema;
	ex->tool_desc.output_schema = ex->manifest.output_schema;
	ex->tool_desc.input_format = "";
	return 0;
}

int ext_unload(struct ext *ex)
{
	if (!ex)
		return -EINVAL;
	if (ex->dl_handle) {
		log_info(ex ? ex->logger : NULL, "ext_unload: dlclose %s", ex->manifest.name);
		dlclose(ex->dl_handle);
		ex->dl_handle = NULL;
	}
	ext_manifest_cleanup(&ex->manifest);
	memset(ex, 0, sizeof(*ex));
	return 0;
}

void ext_manifest_cleanup(struct ext_manifest *m)
{
	if (!m)
		return;
	free(m->input_schema);
	free(m->output_schema);
	for (int i = 0; i < m->fronts_count; i++)
		free(m->fronts[i]);
	free(m->fronts);
	for (int i = 0; i < m->categories_count; i++)
		free(m->categories[i]);
	free(m->categories);
	for (int i = 0; i < m->allowed_paths_count; i++)
		free(m->allowed_paths[i]);
	free(m->allowed_paths);
	for (int i = 0; i < m->allowed_env_count; i++)
		free(m->allowed_env[i]);
	free(m->allowed_env);
	for (int i = 0; i < m->allowed_mach_services_count; i++)
		free(m->allowed_mach_services[i]);
	free(m->allowed_mach_services);
	memset(m, 0, sizeof(*m));
}

int ext_manifest_supports_front(const struct ext_manifest *m,
				const char *front)
{
	if (!m || !front || !front[0])
		return 0;
	if (m->fronts_count == 0)
		return 1;
	for (int i = 0; i < m->fronts_count; i++) {
		if (m->fronts[i] && strcmp(m->fronts[i], front) == 0)
			return 1;
	}
	return 0;
}

void ext_user_data_destroy(void *user_data)
{
	struct ext *ex = (struct ext *)user_data;
	if (!ex)
		return;
	ext_unload(ex);
	free(ex);
}

static char *ext_resolve_sandbox_path(const char *path)
{
	char cwd[PATH_MAX];
	char *expanded;
	char *absolute;
	char *resolved;

	expanded = file_expand_path_copy(path);
	if (!expanded)
		return NULL;
	if (file_path_is_absolute(expanded)) {
		absolute = expanded;
	} else {
		if (!getcwd(cwd, sizeof(cwd))) {
			free(expanded);
			return NULL;
		}
		absolute = file_path_full_alloc(cwd, expanded);
		free(expanded);
		if (!absolute)
			return NULL;
	}
	resolved = file_resolve_path_copy(absolute);
	free(absolute);
	return resolved;
}

void ext_sandbox_config_cleanup(struct sandbox_config *config)
{
	sandbox_config_cleanup(config);
}

int ext_sandbox_config_prepare(const struct ext *ex, struct sandbox_config *config)
{
	if (!ex || !config || ex->manifest.allowed_paths_count < 0)
		return -EINVAL;
	if (config->owned_paths.read.elts || config->owned_paths.write.elts ||
	    config->owned_paths.remove.elts)
		return -EEXIST;
	struct sandbox_config prepared MORPH_CLEANUP(sandbox_config_cleanup) = {0};
	struct sandbox_path_set paths MORPH_CLEANUP(sandbox_path_set_cleanup) = {0};
	int count = ex->manifest.allowed_paths_count;
	int rc = morph_array_init(&paths.read, (size_t)count + 1, sizeof(*prepared.read_paths));
	if (rc == 0)
		rc = morph_array_init(&paths.write, (size_t)count + 1,
				      sizeof(*prepared.read_paths));
	if (rc == 0)
		rc = morph_array_init(&paths.remove, (size_t)count + 1,
				      sizeof(*prepared.read_paths));
	if (rc != 0)
		return rc;
	for (int i = 0; i <= count; i++) {
		const char *source = i ? ex->manifest.allowed_paths[i - 1] : ex->path;
		char *resolved MORPH_CLEANUP(morph_charp_cleanup) =
			ext_resolve_sandbox_path(source);
		if (!resolved)
			return -ENOENT;
		char **slot = morph_array_push_borrow(&paths.read);
		if (!slot)
			return -ENOMEM;
		*slot = morph_charp_steal(&resolved);
		if (i && (ex->manifest.permissions & EXT_PERM_FILESYS)) {
			const char *path = *slot;
			char **write = morph_array_push_borrow(&paths.write);
			if (!write)
				return -ENOMEM;
			*write = strdup(path);
			if (!*write)
				return -ENOMEM;
			char **remove = morph_array_push_borrow(&paths.remove);
			if (!remove)
				return -ENOMEM;
			*remove = strdup(path);
			if (!*remove)
				return -ENOMEM;
		}
	}
	prepared.logger = ex->logger;
	prepared.path_policy_enabled = 1;
	sandbox_config_paths_move(&prepared, &paths);
	prepared.permissions = ex->manifest.permissions;
	prepared.path_policy_enabled = 1;
	prepared.network_access = !!(ex->manifest.permissions & EXT_PERM_NETWORK);
	prepared.process_exec = !!(ex->manifest.permissions & EXT_PERM_EXEC);
	prepared.allow_pty = !!(ex->manifest.permissions & EXT_PERM_PTY);
	prepared.allow_process_info = !!(ex->manifest.permissions & EXT_PERM_PROCESS_INFO);
	prepared.allow_ipc = !!(ex->manifest.permissions & EXT_PERM_IPC);
	prepared.allow_temp = !!(ex->manifest.permissions & EXT_PERM_TEMP);
	prepared.allowed_env = ex->manifest.allowed_env;
	prepared.allowed_env_count = ex->manifest.allowed_env_count;
	prepared.allowed_mach_services = ex->manifest.allowed_mach_services;
	prepared.allowed_mach_services_count = ex->manifest.allowed_mach_services_count;
	prepared.max_memory_mb = ex->manifest.max_memory_mb;
	prepared.max_cpu_seconds = ex->manifest.max_cpu_seconds;
	prepared.max_open_files = ex->manifest.max_open_files;
	*config = prepared;
	memset(&prepared, 0, sizeof(prepared));
	return 0;
}

/**
 * \brief Execute a borrowed extension with optional cooperative cancellation.
 * \param[in] ex: Loaded extension; caller keeps it alive until return.
 * \param[in] args_json: Borrowed JSON input until return.
 * \param[out] result_json: Empty owner slot; caller frees returned JSON, including diagnostics.
 * \param[in] cancelled: Optional synchronous predicate; native code must cooperate.
 * \param[in] user: Borrowed cancellation context until return.
 * \return Zero on success, negative error otherwise; output may contain diagnostics.
 */
int ext_run_bounded(struct ext *ex, const char *args_json, char **result_json,
		       int (*cancelled)(void *), void *user, const struct sandbox_config *limits)
{
	if (!ex || !args_json || !result_json)
		return -EINVAL;
	if (*result_json)
		return -EEXIST;
	if (cancelled != NULL && cancelled(user)) {
		return -ECANCELED;
	}
	if (!ex->enabled)
		return -EACCES;
	if (ex->run)
		return limits ? -ENOTSUP : ex->run(args_json, result_json);

	if (strcmp(ex->manifest.type, "exec") == 0 && ex->exec_path[0]) {
		log_info(ex ? ex->logger : NULL, "executing ext: %s", ex->exec_path);

		int stdin_pipe[2];
		int stdout_pipe[2];
		int stderr_pipe[2];
		int rc;

		if ((rc = morph_io_pipe(stdin_pipe)) != 0) {
			log_err(ex ? ex->logger : NULL, "ext_run: pipe failed");
			return rc;
		}
		if ((rc = morph_io_pipe(stdout_pipe)) != 0) {
			close(stdin_pipe[0]);
			close(stdin_pipe[1]);
			log_err(ex ? ex->logger : NULL, "ext_run: pipe failed");
			return rc;
		}
		if ((rc = morph_io_pipe(stderr_pipe)) != 0) {
			close(stdin_pipe[0]);
			close(stdin_pipe[1]);
			close(stdout_pipe[0]);
			close(stdout_pipe[1]);
			log_err(ex ? ex->logger : NULL, "ext_run: pipe failed");
			return rc;
		}

		pid_t pid = fork();
		if (pid < 0) {
			int err = errno;
			close(stdin_pipe[0]);
			close(stdin_pipe[1]);
			close(stdout_pipe[0]);
			close(stdout_pipe[1]);
			close(stderr_pipe[0]);
			close(stderr_pipe[1]);
			log_err(ex ? ex->logger : NULL, "ext_run: fork failed");
			return -err;
		}

		if (pid == 0) {
			morph_signals_reset_child();
			close(stdin_pipe[1]);
			close(stdout_pipe[0]);
			close(stderr_pipe[0]);
			dup2(stdin_pipe[0], STDIN_FILENO);
			dup2(stdout_pipe[1], STDOUT_FILENO);
			dup2(stderr_pipe[1], STDERR_FILENO);
			close(stdin_pipe[0]);
			close(stdout_pipe[1]);
			close(stderr_pipe[1]);
			if (sandbox_start_isolated_session() != 0)
				_exit(126);
			if (sandbox_close_inherited_fds() != 0)
				_exit(126);

			struct sandbox_config sb_cfg MORPH_CLEANUP(sandbox_config_cleanup) = {0};

			if (ext_sandbox_config_prepare(ex, &sb_cfg) != 0)
				_exit(126);

			if (limits && sandbox_permission_intersect(&sb_cfg, limits) != 0) {
				sandbox_config_cleanup(&sb_cfg);
				_exit(126);
			}

			if (sandbox_enter(&sb_cfg) != 0) {
				sandbox_config_cleanup(&sb_cfg);
				_exit(126);
			}
			sandbox_config_cleanup(&sb_cfg);

			execlp(ex->exec_path, ex->exec_path, (char *)NULL);
			_exit(127);
		}

		close(stdin_pipe[0]);
		close(stdout_pipe[1]);
		close(stderr_pipe[1]);

		struct jsonrpc_request req;
		memset(&req, 0, sizeof(req));
		req.id = 1;
		req.method = "run";
		req.params_json = args_json;

		char *request_str MORPH_CLEANUP(morph_json_text_cleanup) =
			jsonrpc_request_create(&req);
		if (!request_str) {
			close(stdin_pipe[1]);
			close(stdout_pipe[0]);
			close(stderr_pipe[0]);
			kill(pid, SIGKILL);
			while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
			*result_json = strdup("{\"error\":\"failed to build JSON-RPC request\"}");
			return -ENOMEM;
		}

		morph_buf_t request MORPH_CLEANUP(morph_buf_cleanup) = {0};
		morph_buf_t output MORPH_CLEANUP(morph_buf_cleanup) = {0};
		morph_buf_t errors MORPH_CLEANUP(morph_buf_cleanup) = {0};
		rc = morph_buf_init(&request, strlen(request_str) + 2);
		if (rc == 0) {
			rc = morph_buf_puts(&request, request_str);
		}
		if (rc == 0) {
			rc = morph_buf_putc(&request, '\n');
		}
		if (rc == 0) {
			rc = morph_buf_init(&output, BUFSIZ);
		}
		if (rc == 0) {
			rc = morph_buf_init(&errors, BUFSIZ);
		}
		morph_json_text_cleanup(&request_str);
		int status = 0;
		if (rc == 0) {
			rc = morph_child_exchange(pid, stdin_pipe[1], stdout_pipe[0],
						  stderr_pipe[0], morph_buf_cstr_borrow(&request),
						  request.len, 300000, 1024 * 1024, cancelled, user,
						  &output, &errors, &status);
		} else {
			close(stdin_pipe[1]);
			kill(pid, SIGKILL);
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
		}
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);
		morph_buf_cleanup(&request);
		char *raw_response = morph_buf_detach(&output);
		char *child_stderr = morph_buf_detach(&errors);

		if (child_stderr && child_stderr[0]) {
			log_warn(ex ? ex->logger : NULL, "ext %s stderr: %s", ex->manifest.name,
				 child_stderr);
		}

		if (rc < 0 || !raw_response) {
			free(child_stderr);
			free(raw_response);
			*result_json = strdup("{\"error\":\"failed to read ext output\"}");
			return rc < 0 ? rc : -EIO;
		}

		/* If the child was killed (sandbox), report that */
		if (WIFSIGNALED(status)) {
			log_warn(ex ? ex->logger : NULL, "ext %s killed by signal %d",
				 ex->manifest.name, WTERMSIG(status));
			free(child_stderr);
			free(raw_response);
			*result_json = strdup("{\"error\":\"ext process was terminated\"}");
			return MORPH_ERR_SANDBOX;
		}
		if (WIFEXITED(status) && WEXITSTATUS(status) != 0 &&
		    raw_response[0] == '\0') {
			int exit_code = WEXITSTATUS(status);

			free(child_stderr);
			free(raw_response);
			*result_json = strdup(exit_code == 126 ?
				"{\"error\":\"extension sandbox setup failed\"}" :
				"{\"error\":\"extension process exited before "
				"producing a JSON-RPC response\"}");
			if (exit_code == 126)
				return MORPH_ERR_SANDBOX;
			return -ECHILD;
		}
		free(child_stderr);

		/* Parse JSON-RPC response and extract result field */
		struct jsonrpc_response jr = {0};
		int parse_rc = jsonrpc_response_copy(raw_response, &jr);
		free(raw_response);

		if (parse_rc < 0) {
			*result_json = strdup("{\"error\":\"invalid JSON-RPC response\"}");
			return MORPH_ERR_PARSE;
		}

		if (jr.has_error) {
			cJSON *error = cJSON_CreateObject();
			if (error != NULL &&
			    cJSON_AddStringToObject(error, "error",
						    jr.error_message != NULL
							    ? jr.error_message
							    : "Extension protocol error")) {
				char *text MORPH_CLEANUP(morph_json_text_cleanup) =
					morph_json_print_copy(error);
				if (text)
					*result_json = strdup(text);
			}
			cJSON_Delete(error);
			jsonrpc_response_free(&jr);
			return *result_json == NULL ? -ENOMEM : MORPH_ERR_PROTOCOL;
		}

		*result_json = strdup(jr.result_json ? jr.result_json : "{}");
		jsonrpc_response_free(&jr);
		return *result_json ? 0 : -ENOMEM;
	}

	log_info(ex ? ex->logger : NULL, "ext_run: no run function for %s", ex->manifest.name);
	return MORPH_ERR_NOT_CONFIGURED;
}

/** \brief Execute an extension with the legacy non-cancellable contract. */
int ext_run_cancelable(struct ext *ex, const char *args_json, char **result_json,
	int (*cancelled)(void *), void *user)
{
	return ext_run_bounded(ex, args_json, result_json, cancelled, user, NULL);
}

int ext_run(struct ext *ex, const char *args_json, char **result_json)
{
	return ext_run_cancelable(ex, args_json, result_json, NULL, NULL);
}
