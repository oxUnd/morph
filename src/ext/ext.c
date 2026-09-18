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

int ext_load(struct ext *ex, const char *dir_path)
{
	if (!ex || !dir_path)
		return -EINVAL;
	memset(ex, 0, sizeof(*ex));
	strncpy(ex->path, dir_path, sizeof(ex->path) - 1);

	char manifest_path[PATH_MAX];
	snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.toml", dir_path);

	struct stat st;
	if (stat(manifest_path, &st) != 0) {
		snprintf(manifest_path, sizeof(manifest_path),
			 "%s/morph-ext.toml", dir_path);
		if (stat(manifest_path, &st) != 0) {
			log_err("ext manifest not found in: %s", dir_path);
			return -ENOENT;
		}
	}

	int rc = manifest_parse_file(manifest_path, &ex->manifest);
	if (rc < 0) {
		log_err("failed to parse ext manifest: %s", manifest_path);
		return rc;
	}

	if (strcmp(ex->manifest.type, "exec") == 0) {
		rc = ext_load_exec(ex, dir_path);
		if (rc < 0) {
			log_err("ext_load: failed to load exec ext %s: %d",
				ex->manifest.name, rc);
			return rc;
		}
	} else if (strcmp(ex->manifest.type, "so") == 0) {
		rc = ext_load_so(ex, dir_path);
		if (rc < 0) {
			log_err("ext_load: failed to load so ext %s: %d",
				ex->manifest.name, rc);
			return rc;
		}
	} else {
		log_warn("unknown ext type: %s", ex->manifest.type);
	}

	snprintf(ex->tool_desc.name, sizeof(ex->tool_desc.name), "%s", ex->manifest.name);
	snprintf(ex->tool_desc.description, sizeof(ex->tool_desc.description), "%s", ex->manifest.description);
	if (!ex->manifest.input_schema || !ex->manifest.output_schema)
		return -EINVAL;
	if (ex->manifest.input_schema)
		snprintf(ex->tool_desc.input_schema, sizeof(ex->tool_desc.input_schema),
			 "%s", ex->manifest.input_schema);
	if (ex->manifest.output_schema)
		snprintf(ex->tool_desc.output_schema, sizeof(ex->tool_desc.output_schema),
			 "%s", ex->manifest.output_schema);

	ex->enabled = 1;
	return 0;
}

int ext_unload(struct ext *ex)
{
	if (!ex)
		return -EINVAL;
	if (ex->dl_handle) {
		log_info("ext_unload: dlclose %s", ex->manifest.name);
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

static int read_fd(int fd, char **out, size_t *out_len)
{
	morph_buf_t buf;
	int rc = morph_buf_init(&buf, 8192);
	if (rc != 0)
		return rc;

	while (1) {
		if (buf.len + 4096 > buf.cap) {
			int grow_rc = morph_buf_reserve(&buf, buf.cap);
			if (grow_rc != 0) {
				morph_buf_cleanup(&buf);
				return -ENOMEM;
			}
		}
		ssize_t n = read(fd, buf.data + buf.len,
				 buf.cap - buf.len - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			morph_buf_cleanup(&buf);
			return -EIO;
		}
		if (n == 0)
			break;
		buf.len += (size_t)n;
	}
	buf.data[buf.len] = '\0';
	size_t save_len = buf.len;
	*out = morph_buf_detach(&buf);
	if (out_len)
		*out_len = save_len;
	return 0;
}

static char *ext_resolve_sandbox_path(const char *path)
{
	char cwd[PATH_MAX];
	char *expanded;
	char *absolute;
	char *resolved;

	expanded = file_expand_path(path);
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
	resolved = file_resolve_path(absolute);
	free(absolute);
	return resolved;
}

void ext_sandbox_config_cleanup(struct sandbox_config *config)
{
	if (!config)
		return;
	for (int i = 0; i < config->read_paths_count; i++)
		free(config->read_paths[i]);
	free(config->read_paths);
	memset(config, 0, sizeof(*config));
}

int ext_sandbox_config_prepare(const struct ext *ex,
			       struct sandbox_config *config)
{
	char **read_paths;
	int count;

	if (!ex || !config)
		MORPH_RETURN(-EINVAL);
	count = ex->manifest.allowed_paths_count;
	read_paths = calloc((size_t)count + 1, sizeof(*read_paths));
	if (!read_paths)
		MORPH_RETURN(-ENOMEM);
	memset(config, 0, sizeof(*config));
	config->read_paths = read_paths;
	read_paths[0] = ext_resolve_sandbox_path(ex->path);
	if (!read_paths[0]) {
		ext_sandbox_config_cleanup(config);
		MORPH_RETURN(-ENOENT);
	}
	config->read_paths_count = 1;
	for (int i = 0; i < count; i++) {
		read_paths[i + 1] = ext_resolve_sandbox_path(
			ex->manifest.allowed_paths[i]);
		if (!read_paths[i + 1]) {
			ext_sandbox_config_cleanup(config);
			MORPH_RETURN(-ENOENT);
		}
		config->read_paths_count++;
	}

	config->permissions = ex->manifest.permissions;
	config->path_policy_enabled = 1;
	if (ex->manifest.permissions & EXT_PERM_FILESYS) {
		config->write_paths = read_paths + 1;
		config->write_paths_count = count;
		config->delete_paths = read_paths + 1;
		config->delete_paths_count = count;
	}
	config->network_access =
		!!(ex->manifest.permissions & EXT_PERM_NETWORK);
	config->process_exec = !!(ex->manifest.permissions & EXT_PERM_EXEC);
	config->allow_pty = !!(ex->manifest.permissions & EXT_PERM_PTY);
	config->allow_process_info =
		!!(ex->manifest.permissions & EXT_PERM_PROCESS_INFO);
	config->allow_ipc = !!(ex->manifest.permissions & EXT_PERM_IPC);
	config->allow_temp = !!(ex->manifest.permissions & EXT_PERM_TEMP);
	config->allowed_env = ex->manifest.allowed_env;
	config->allowed_env_count = ex->manifest.allowed_env_count;
	config->allowed_mach_services = ex->manifest.allowed_mach_services;
	config->allowed_mach_services_count =
		ex->manifest.allowed_mach_services_count;
	config->max_memory_mb = ex->manifest.max_memory_mb;
	config->max_cpu_seconds = ex->manifest.max_cpu_seconds;
	config->max_open_files = ex->manifest.max_open_files;
	return 0;
}

int ext_run(struct ext *ex, const char *args_json, char **result_json)
{
	if (!ex || !args_json || !result_json)
		return -EINVAL;
	if (!ex->enabled)
		return -EACCES;
	if (ex->run)
		return ex->run(args_json, result_json);

	if (strcmp(ex->manifest.type, "exec") == 0 && ex->exec_path[0]) {
		log_info("executing ext: %s", ex->exec_path);

		int stdin_pipe[2];
		int stdout_pipe[2];
		int stderr_pipe[2];

		if (pipe(stdin_pipe) < 0) {
			log_err("ext_run: pipe failed");
			MORPH_RETURN_ERRNO();
		}
		if (pipe(stdout_pipe) < 0) {
			int err = errno;
			close(stdin_pipe[0]);
			close(stdin_pipe[1]);
			log_err("ext_run: pipe failed");
			MORPH_RETURN(-err);
		}
		if (pipe(stderr_pipe) < 0) {
			int err = errno;
			close(stdin_pipe[0]);
			close(stdin_pipe[1]);
			close(stdout_pipe[0]);
			close(stdout_pipe[1]);
			log_err("ext_run: pipe failed");
			MORPH_RETURN(-err);
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
			log_err("ext_run: fork failed");
			MORPH_RETURN(-err);
		}

		if (pid == 0) {
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
			log_shutdown();
			if (sandbox_close_inherited_fds() != 0)
				_exit(126);

			struct sandbox_config sb_cfg;

			if (ext_sandbox_config_prepare(ex, &sb_cfg) != 0)
				_exit(126);
			if (sandbox_enter(&sb_cfg) != 0) {
				ext_sandbox_config_cleanup(&sb_cfg);
				_exit(126);
			}
			ext_sandbox_config_cleanup(&sb_cfg);

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

		char *request_str = jsonrpc_build_request(&req);
		if (!request_str) {
			close(stdin_pipe[1]);
			close(stdout_pipe[0]);
			close(stderr_pipe[0]);
			waitpid(pid, NULL, 0);
			*result_json = strdup("{\"error\":\"failed to build JSON-RPC request\"}");
			return -ENOMEM;
		}

		size_t req_len = strlen(request_str);
		ssize_t written = 0;
		while ((size_t)written < req_len) {
			ssize_t n = write(stdin_pipe[1],
					  request_str + written,
					  req_len - (size_t)written);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				free(request_str);
				close(stdin_pipe[1]);
				close(stdout_pipe[0]);
				close(stderr_pipe[0]);
				waitpid(pid, NULL, 0);
				*result_json = strdup("{\"error\":\"failed to write to ext\"}");
				return -EIO;
			}
			written += n;
		}
		/* JSON-RPC is line-based: terminate with newline */
		write(stdin_pipe[1], "\n", 1);
		close(stdin_pipe[1]);
		free(request_str);

		char *raw_response = NULL;
		int rc = read_fd(stdout_pipe[0], &raw_response, NULL);
		close(stdout_pipe[0]);

		char *child_stderr = NULL;
		read_fd(stderr_pipe[0], &child_stderr, NULL);
		close(stderr_pipe[0]);

		int status;
		waitpid(pid, &status, 0);

		if (child_stderr && child_stderr[0]) {
			log_warn("ext %s stderr: %s",
				 ex->manifest.name, child_stderr);
		}

		if (rc < 0 || !raw_response) {
			free(child_stderr);
			free(raw_response);
			*result_json = strdup("{\"error\":\"failed to read ext output\"}");
			return -EIO;
		}

		/* If the child was killed (sandbox), report that */
		if (WIFSIGNALED(status)) {
			log_warn("ext %s killed by signal %d",
				 ex->manifest.name, WTERMSIG(status));
			free(child_stderr);
			free(raw_response);
			*result_json = strdup("{\"error\":\"ext process was terminated\"}");
			MORPH_RETURN(MORPH_ERR_SANDBOX);
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
				MORPH_RETURN(MORPH_ERR_SANDBOX);
			MORPH_RETURN(-ECHILD);
		}
		free(child_stderr);

		/* Parse JSON-RPC response and extract result field */
		struct jsonrpc_response jr;
		int parse_rc = jsonrpc_parse_response(raw_response, &jr);
		free(raw_response);

		if (parse_rc < 0) {
			*result_json = strdup("{\"error\":\"invalid JSON-RPC response\"}");
			MORPH_RETURN(MORPH_ERR_PARSE);
		}

		if (jr.has_error) {
			char err_buf[1024];
			snprintf(err_buf, sizeof(err_buf),
				 "{\"error\":\"ext error: %s (code %d)\"}",
				 jr.error_message ? jr.error_message : "unknown",
				 jr.error_code);
			*result_json = strdup(err_buf);
			jsonrpc_response_free(&jr);
			MORPH_RETURN(MORPH_ERR_PROTOCOL);
		}

		*result_json = jr.result_json ? jr.result_json : strdup("{}");
		/* ownership transferred, do not free result_json */
		jr.result_json = NULL;
		jsonrpc_response_free(&jr);
		return 0;
	}

	log_info("ext_run: no run function for %s", ex->manifest.name);
	MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
}
