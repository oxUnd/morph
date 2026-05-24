#include "ext.h"
#include "loader.h"
#include "util/log.h"
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
#include "util/error.h"

int ext_load(struct ext *ex, const char *dir_path)
{
	if (!ex || !dir_path)
		return -EINVAL;
	memset(ex, 0, sizeof(*ex));
	strncpy(ex->path, dir_path, sizeof(ex->path) - 1);

	char manifest_path[1024];
	snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.toml", dir_path);

	struct stat st;
	if (stat(manifest_path, &st) != 0) {
		log_err("ext manifest not found: %s", manifest_path);
		return -ENOENT;
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
	snprintf(ex->tool_desc.desc, sizeof(ex->tool_desc.desc), "%s", ex->manifest.description);
	if (ex->manifest.args_schema)
		snprintf(ex->tool_desc.args_spec, sizeof(ex->tool_desc.args_spec),
			 "%s", ex->manifest.args_schema);

	ex->enabled = 1;
	return 0;
}

int ext_unload(struct ext *ex)
{
	if (!ex)
		return -EINVAL;
	if (ex->dl_handle) {
		log_info("ext_unload: would dlclose %s", ex->manifest.name);
		ex->dl_handle = NULL;
	}
	free(ex->manifest.args_schema);
	free(ex->manifest.output_schema);
	for (int i = 0; i < ex->manifest.allowed_paths_count; i++)
		free(ex->manifest.allowed_paths[i]);
	free(ex->manifest.allowed_paths);
	for (int i = 0; i < ex->manifest.allowed_env_count; i++)
		free(ex->manifest.allowed_env[i]);
	free(ex->manifest.allowed_env);
	memset(ex, 0, sizeof(*ex));
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
	size_t cap = 8192;
	size_t len = 0;
	char *buf = malloc(cap);
	if (!buf)
		return -ENOMEM;

	while (1) {
		if (len + 4096 > cap) {
			cap *= 2;
			char *new_buf = realloc(buf, cap);
			if (!new_buf) {
				free(buf);
				return -ENOMEM;
			}
			buf = new_buf;
		}
		ssize_t n = read(fd, buf + len, cap - len - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			return -EIO;
		}
		if (n == 0)
			break;
		len += (size_t)n;
	}
	buf[len] = '\0';
	*out = buf;
	if (out_len)
		*out_len = len;
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
			MORPH_RETURN(-errno);
		}
		if (pipe(stdout_pipe) < 0) {
			close(stdin_pipe[0]);
			close(stdin_pipe[1]);
			log_err("ext_run: pipe failed");
			MORPH_RETURN(-errno);
		}
		if (pipe(stderr_pipe) < 0) {
			close(stdin_pipe[0]);
			close(stdin_pipe[1]);
			close(stdout_pipe[0]);
			close(stdout_pipe[1]);
			log_err("ext_run: pipe failed");
			MORPH_RETURN(-errno);
		}

		pid_t pid = fork();
		if (pid < 0) {
			close(stdin_pipe[0]);
			close(stdin_pipe[1]);
			close(stdout_pipe[0]);
			close(stdout_pipe[1]);
			close(stderr_pipe[0]);
			close(stderr_pipe[1]);
			log_err("ext_run: fork failed");
			MORPH_RETURN(-errno);
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

			struct sandbox_config sb_cfg;
			memset(&sb_cfg, 0, sizeof(sb_cfg));
			sb_cfg.permissions = ex->manifest.permissions;
			sb_cfg.max_memory_mb = ex->manifest.max_memory_mb;
			sb_cfg.max_cpu_seconds = ex->manifest.max_cpu_seconds;
			sb_cfg.allowed_paths = ex->manifest.allowed_paths;
			sb_cfg.allowed_paths_count = ex->manifest.allowed_paths_count;
			sb_cfg.allowed_env = ex->manifest.allowed_env;
			sb_cfg.allowed_env_count = ex->manifest.allowed_env_count;
			sb_cfg.max_open_files = ex->manifest.max_open_files;
			sandbox_enter(&sb_cfg);

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
		free(child_stderr);

		if (rc < 0 || !raw_response) {
			free(raw_response);
			*result_json = strdup("{\"error\":\"failed to read ext output\"}");
			return -EIO;
		}

		/* If the child was killed (sandbox), report that */
		if (WIFSIGNALED(status)) {
			log_warn("ext %s killed by signal %d",
				 ex->manifest.name, WTERMSIG(status));
			free(raw_response);
			*result_json = strdup("{\"error\":\"ext process was terminated\"}");
			MORPH_RETURN(MORPH_ERR_SANDBOX);
		}

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
