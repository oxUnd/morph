#include "skill.h"
#include "loader.h"
#include "util/log.h"
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

int skill_load(struct skill *sk, const char *dir_path)
{
	if (!sk || !dir_path)
		return -EINVAL;
	memset(sk, 0, sizeof(*sk));
	strncpy(sk->path, dir_path, sizeof(sk->path) - 1);

	char manifest_path[1024];
	snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.toml", dir_path);

	struct stat st;
	if (stat(manifest_path, &st) != 0) {
		log_err("skill manifest not found: %s", manifest_path);
		return -ENOENT;
	}

	int rc = manifest_parse_file(manifest_path, &sk->manifest);
	if (rc < 0) {
		log_err("failed to parse skill manifest: %s", manifest_path);
		return rc;
	}

	if (strcmp(sk->manifest.type, "exec") == 0) {
		rc = skill_load_exec(sk, dir_path);
		if (rc < 0) {
			log_err("skill_load: failed to load exec skill %s: %d",
				sk->manifest.name, rc);
			return rc;
		}
	} else if (strcmp(sk->manifest.type, "so") == 0) {
		rc = skill_load_so(sk, dir_path);
		if (rc < 0) {
			log_err("skill_load: failed to load so skill %s: %d",
				sk->manifest.name, rc);
			return rc;
		}
	} else {
		log_warn("unknown skill type: %s", sk->manifest.type);
	}

	snprintf(sk->tool_desc.name, sizeof(sk->tool_desc.name), "%s", sk->manifest.name);
	snprintf(sk->tool_desc.desc, sizeof(sk->tool_desc.desc), "%s", sk->manifest.description);
	if (sk->manifest.args_schema)
		snprintf(sk->tool_desc.args_spec, sizeof(sk->tool_desc.args_spec),
			 "%s", sk->manifest.args_schema);

	sk->enabled = 1;
	return 0;
}

int skill_unload(struct skill *sk)
{
	if (!sk)
		return -EINVAL;
	if (sk->dl_handle) {
		log_info("skill_unload: would dlclose %s", sk->manifest.name);
		sk->dl_handle = NULL;
	}
	free(sk->manifest.args_schema);
	free(sk->manifest.output_schema);
	for (int i = 0; i < sk->manifest.allowed_paths_count; i++)
		free(sk->manifest.allowed_paths[i]);
	free(sk->manifest.allowed_paths);
	for (int i = 0; i < sk->manifest.allowed_env_count; i++)
		free(sk->manifest.allowed_env[i]);
	free(sk->manifest.allowed_env);
	memset(sk, 0, sizeof(*sk));
	return 0;
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

int skill_run(struct skill *sk, const char *args_json, char **result_json)
{
	if (!sk || !args_json || !result_json)
		return -EINVAL;
	if (!sk->enabled)
		return -EACCES;
	if (sk->run)
		return sk->run(args_json, result_json);

	if (strcmp(sk->manifest.type, "exec") == 0 && sk->exec_path[0]) {
		log_info("executing skill: %s", sk->exec_path);

		int stdin_pipe[2];
		int stdout_pipe[2];

		if (pipe(stdin_pipe) < 0) {
			log_err("skill_run: pipe failed");
			return -EIO;
		}
		if (pipe(stdout_pipe) < 0) {
			close(stdin_pipe[0]);
			close(stdin_pipe[1]);
			log_err("skill_run: pipe failed");
			return -EIO;
		}

		pid_t pid = fork();
		if (pid < 0) {
			close(stdin_pipe[0]);
			close(stdin_pipe[1]);
			close(stdout_pipe[0]);
			close(stdout_pipe[1]);
			log_err("skill_run: fork failed");
			return -EIO;
		}

		if (pid == 0) {
			close(stdin_pipe[1]);
			close(stdout_pipe[0]);
			dup2(stdin_pipe[0], STDIN_FILENO);
			dup2(stdout_pipe[1], STDOUT_FILENO);
			close(stdin_pipe[0]);
			close(stdout_pipe[1]);

			struct sandbox_config sb_cfg;
			memset(&sb_cfg, 0, sizeof(sb_cfg));
			sb_cfg.permissions = sk->manifest.permissions;
			sb_cfg.max_memory_mb = sk->manifest.max_memory_mb;
			sb_cfg.max_cpu_seconds = sk->manifest.max_cpu_seconds;
			sb_cfg.allowed_paths = sk->manifest.allowed_paths;
			sb_cfg.allowed_paths_count = sk->manifest.allowed_paths_count;
			sandbox_enter(&sb_cfg);

			execlp(sk->exec_path, sk->exec_path, (char *)NULL);
			_exit(127);
		}

		close(stdin_pipe[0]);
		close(stdout_pipe[1]);

		struct jsonrpc_request req;
		memset(&req, 0, sizeof(req));
		req.id = 1;
		req.method = "run";
		req.params_json = args_json;

		char *request_str = jsonrpc_build_request(&req);
		if (!request_str) {
			close(stdin_pipe[1]);
			close(stdout_pipe[0]);
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
				waitpid(pid, NULL, 0);
				*result_json = strdup("{\"error\":\"failed to write to skill\"}");
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

		int status;
		waitpid(pid, &status, 0);

		if (rc < 0 || !raw_response) {
			free(raw_response);
			*result_json = strdup("{\"error\":\"failed to read skill output\"}");
			return -EIO;
		}

		/* If the child was killed (sandbox), report that */
		if (WIFSIGNALED(status)) {
			log_warn("skill %s killed by signal %d",
				 sk->manifest.name, WTERMSIG(status));
			free(raw_response);
			*result_json = strdup("{\"error\":\"skill process was terminated\"}");
			return -EIO;
		}

		/* Parse JSON-RPC response and extract result field */
		struct jsonrpc_response jr;
		int parse_rc = jsonrpc_parse_response(raw_response, &jr);
		free(raw_response);

		if (parse_rc < 0) {
			*result_json = strdup("{\"error\":\"invalid JSON-RPC response\"}");
			return -EIO;
		}

		if (jr.has_error) {
			char err_buf[1024];
			snprintf(err_buf, sizeof(err_buf),
				 "{\"error\":\"skill error: %s (code %d)\"}",
				 jr.error_message ? jr.error_message : "unknown",
				 jr.error_code);
			*result_json = strdup(err_buf);
			jsonrpc_response_free(&jr);
			return -EIO;
		}

		*result_json = jr.result_json ? jr.result_json : strdup("{}");
		/* ownership transferred, do not free result_json */
		jr.result_json = NULL;
		jsonrpc_response_free(&jr);
		return 0;
	}

	log_info("skill_run: no run function for %s", sk->manifest.name);
	return -ENOSYS;
}
