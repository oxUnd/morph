#include "dynamic_tool_runner.h"
#include "util/signals.h"
#include "util/child_io.h"
#include "sandbox/sandbox.h"
#include "sandbox/permission.h"
#include "agent/tool_runtime.h"
#include "ipc/jsonrpc.h"
#include "util/buf.h"
#include "util/data.h"
#include "util/error.h"
#include "util/cleanup.h"
#include "util/file.h"
#include "util/name.h"
#include "util/log.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <dlfcn.h>
#endif

static const char *runner_path(char buffer[PATH_MAX]);
static const struct config_dynamic_tool_profile *
active_profile(const struct config_dynamic_tools *cfg);
static int append_list(morph_buf_t *buf, const char *value, int *first);
static char *join_caps(char caps[][DYNAMIC_TOOL_CAP_LEN_MAX], int count);
static char *join_allow(const char values[][DYNAMIC_TOOL_ALLOW_LEN_MAX], int count);
static char *join_path_allow(const char values[][DYNAMIC_TOOL_ALLOW_LEN_MAX], int count,
			     const char *root);
static int effective_caps(struct dynamic_tool *dt,
			  char out[DYNAMIC_TOOL_CAP_MAX][DYNAMIC_TOOL_CAP_LEN_MAX], int *out_count);
static int set_child_env(struct dynamic_tool *dt);

static const char *runner_path(char buffer[PATH_MAX])
{
#ifdef __ANDROID__
	{
		Dl_info info;
		const char *slash;
		size_t dir_len;

		memset(&info, 0, sizeof(info));
		if (dladdr((void *)(uintptr_t)runner_path, &info) != 0 && info.dli_fname &&
		    *info.dli_fname) {
			slash = strrchr(info.dli_fname, '/');
			if (slash) {
				dir_len = (size_t)(slash - info.dli_fname);
				if (dir_len > 0 && dir_len < PATH_MAX) {
					memcpy(buffer, info.dli_fname, dir_len);
					buffer[dir_len] = '\0';
					if (snprintf(buffer + dir_len, PATH_MAX - dir_len,
						     "/libmorph-js-runner.so") <
					    (int)(PATH_MAX - dir_len)) {
						return buffer;
					}
					buffer[0] = '\0';
				}
			}
		}
	}
#endif
	if (morph_executable_find(buffer, PATH_MAX, "morph-js-runner") == 0)
		return buffer;
	return "morph-js-runner";
}

static const struct config_dynamic_tool_profile *
active_profile(const struct config_dynamic_tools *cfg)
{
	if (!cfg)
		return NULL;
	if (strcmp(cfg->mode, "server") == 0)
		return &cfg->server;
	return &cfg->local;
}

static int append_list(morph_buf_t *buf, const char *value, int *first)
{
	int rc;

	if (!value || !*value)
		return 0;
	if (!*first) {
		rc = morph_buf_putc(buf, ',');
		if (rc != 0)
			return rc;
	}
	*first = 0;
	return morph_buf_puts(buf, value);
}

static char *join_caps(char caps[][DYNAMIC_TOOL_CAP_LEN_MAX], int count)
{
	morph_buf_t buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int first = 1;

	if (morph_buf_init(&buf, 128) != 0)
		return NULL;
	for (int i = 0; i < count; i++) {
		if (append_list(&buf, caps[i], &first) != 0) {
			morph_buf_cleanup(&buf);
			return NULL;
		}
	}
	return morph_buf_detach(&buf);
}

static char *join_allow(const char values[][DYNAMIC_TOOL_ALLOW_LEN_MAX], int count)
{
	morph_buf_t buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int first = 1;

	if (morph_buf_init(&buf, 128) != 0)
		return NULL;
	for (int i = 0; i < count; i++) {
		if (append_list(&buf, values[i], &first) != 0) {
			morph_buf_cleanup(&buf);
			return NULL;
		}
	}
	return morph_buf_detach(&buf);
}

static char *join_path_allow(const char values[][DYNAMIC_TOOL_ALLOW_LEN_MAX], int count,
			     const char *root)
{
	morph_buf_t buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int first = 1;

	if (morph_buf_init(&buf, 128) != 0)
		return NULL;
	for (int i = 0; i < count; i++) {
		char *candidate;
		char *resolved;
		int rc;

		if (!values[i][0])
			continue;
		if (strcmp(values[i], "*") == 0) {
			rc = append_list(&buf, values[i], &first);
		} else {
			candidate = file_path_is_absolute(values[i])
					    ? strdup(values[i])
					    : file_path_full_alloc(root, values[i]);
			if (!candidate) {
				morph_buf_cleanup(&buf);
				return NULL;
			}
			resolved = file_resolve_path_copy(candidate);
			free(candidate);
			if (!resolved) {
				morph_buf_cleanup(&buf);
				return NULL;
			}
			rc = append_list(&buf, resolved, &first);
			free(resolved);
		}
		if (rc != 0) {
			morph_buf_cleanup(&buf);
			return NULL;
		}
	}
	return morph_buf_detach(&buf);
}

static int effective_caps(struct dynamic_tool *dt,
			  char out[DYNAMIC_TOOL_CAP_MAX][DYNAMIC_TOOL_CAP_LEN_MAX], int *out_count)
{
	const struct config_dynamic_tool_profile *profile;
	int count = 0;

	if (!dt || !dt->cfg || !out || !out_count)
		return -EINVAL;
	profile = active_profile(dt->cfg);
	if (!profile)
		return -EINVAL;
	for (int i = 0; i < profile->default_capabilities_count && count < DYNAMIC_TOOL_CAP_MAX;
	     i++) {
		strncpy(out[count], profile->default_capabilities[i], DYNAMIC_TOOL_CAP_LEN_MAX - 1);
		count++;
	}
	*out_count = count;
	return 0;
}

void dynamic_tool_runner_destroy(void *opaque)
{
	struct dynamic_tool *tool = opaque;
	if (tool != NULL) {
		free(tool->source);
		free(tool->description);
		free(tool->input_schema);
		free(tool->output_schema);
		free(tool);
	}
}

int dynamic_tool_runner_cancelled(void *opaque)
{
	const struct tool_result *result = opaque;
	return result->runtime != NULL && http_wait_cancelable(&result->runtime->http, 0) != 0;
}

static int set_child_env(struct dynamic_tool *dt)
{
	const struct config_dynamic_tool_profile *profile;
	const char *workdir;
	const char *output_dir;
	char caps[DYNAMIC_TOOL_CAP_MAX][DYNAMIC_TOOL_CAP_LEN_MAX];
	int caps_count = 0;
	char *cap_str;
	char *read_str;
	char *write_str;
	char *command_str;
	char *network_str;
	char timeout_buf[32];
	char capture_buf[32];
	int rc;

	if (!dt || !dt->tctx)
		return -EINVAL;
	workdir = tool_context_workdir_borrow(dt->tctx);
	output_dir = tool_context_output_dir_borrow(dt->tctx);
	if (!workdir || !*workdir || !output_dir || !*output_dir)
		return -EINVAL;
	profile = active_profile(dt->cfg);
	rc = effective_caps(dt, caps, &caps_count);
	if (rc < 0)
		return rc;
	cap_str = join_caps(caps, caps_count);
	read_str = join_path_allow(profile->allowed_read_paths, profile->allowed_read_paths_count,
				   workdir);
	write_str = join_path_allow(profile->allowed_write_paths,
				    profile->allowed_write_paths_count, output_dir);
	command_str = join_allow(profile->allowed_commands, profile->allowed_commands_count);
	network_str = join_allow(profile->allowed_network, profile->allowed_network_count);
	if (!cap_str || !read_str || !write_str || !command_str || !network_str) {
		free(cap_str);
		free(read_str);
		free(write_str);
		free(command_str);
		free(network_str);
		return -ENOMEM;
	}
	snprintf(timeout_buf, sizeof(timeout_buf), "%d",
		 dt->cfg->default_timeout_seconds > 0 ? dt->cfg->default_timeout_seconds : 30);
	snprintf(capture_buf, sizeof(capture_buf), "%d",
		 dt->cfg->default_max_output_bytes > 0
			? dt->cfg->default_max_output_bytes : DYN_TOOL_OUTPUT_LIMIT);
	rc = 0;
	if (setenv("MORPH_DYNAMIC_CAPS", cap_str, 1) != 0 ||
	    setenv("MORPH_DYNAMIC_ALLOWED_READ", read_str, 1) != 0 ||
	    setenv("MORPH_DYNAMIC_ALLOWED_WRITE", write_str, 1) != 0 ||
	    setenv("MORPH_DYNAMIC_ALLOWED_COMMANDS", command_str, 1) != 0 ||
	    setenv("MORPH_DYNAMIC_ALLOWED_NETWORK", network_str, 1) != 0 ||
	    setenv("MORPH_DYNAMIC_TIMEOUT", timeout_buf, 1) != 0 ||
	    setenv("MORPH_RUNNER_MAX_CAPTURE_BYTES", capture_buf, 1) != 0 ||
	    setenv("MORPH_DYNAMIC_WORKDIR", workdir, 1) != 0 ||
	    setenv("MORPH_DYNAMIC_OUTPUT_DIR", output_dir, 1) != 0)
		rc = -errno;
	free(cap_str);
	free(read_str);
	free(write_str);
	free(command_str);
	free(network_str);
	return rc;
}

int dynamic_tool_runner_execute(const char *args_json, struct tool_result *result, void *user_data)
{
	struct dynamic_tool *dt = user_data;
	int stdin_pipe[2];
	int stdout_pipe[2];
	int stderr_pipe[2];
	pid_t pid;
	char *request_str = NULL;
	morph_buf_t out MORPH_CLEANUP(morph_buf_cleanup) = {0};
	morph_buf_t err MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc;

	int status = 0;

	if (!dt || !result)
		return -EINVAL;
	size_t output_limit = dt->cfg->default_max_output_bytes > 0
		? (size_t)dt->cfg->default_max_output_bytes : DYN_TOOL_OUTPUT_LIMIT;
	char snapshot[] = "/tmp/morph-tool-XXXXXX";
	int snapshot_fd = mkstemp(snapshot);
	if (snapshot_fd < 0) {
		return -errno;
	}
	rc = dt->source != NULL ? file_write_all(snapshot, dt->source, strlen(dt->source))
				: -ENOENT;
	if (rc != 0) {
		close(snapshot_fd);
		unlink(snapshot);
		return rc;
	}
	close(snapshot_fd);

	if ((rc = morph_io_pipe(stdin_pipe)) != 0) {
		unlink(snapshot);
		return rc;
	}
	if ((rc = morph_io_pipe(stdout_pipe)) != 0) {
		close(stdin_pipe[0]);
		close(stdin_pipe[1]);
		unlink(snapshot);
		return rc;
	}
	if ((rc = morph_io_pipe(stderr_pipe)) != 0) {
		close(stdin_pipe[0]);
		close(stdin_pipe[1]);
		close(stdout_pipe[0]);
		close(stdout_pipe[1]);
		unlink(snapshot);
		return rc;
	}
	struct sandbox_config sandbox MORPH_CLEANUP(sandbox_config_cleanup) = {0};
	morph_array_t read_views MORPH_CLEANUP(morph_array_cleanup) = {0};
	char runner_storage[PATH_MAX];
	const char *runner = runner_path(runner_storage);

	rc = sandbox_permission_prepare(dt->tctx, dt->name, NULL, &sandbox);
	if (rc != 0) {
		close(stdin_pipe[0]);
		close(stdin_pipe[1]);
		close(stdout_pipe[0]);
		close(stdout_pipe[1]);
		close(stderr_pipe[0]);
		close(stderr_pipe[1]);
		unlink(snapshot);
		return rc;
	}
	if (!sandbox.read_all) {
		rc = morph_array_init(&read_views, (size_t)sandbox.read_paths_count + 2,
				      sizeof(char *));
		char **reads = rc == 0 ? morph_array_push_n_borrow(
						 &read_views, (size_t)sandbox.read_paths_count + 2)
				       : NULL;

		if (!reads) {
			close(stdin_pipe[0]);
			close(stdin_pipe[1]);
			close(stdout_pipe[0]);
			close(stdout_pipe[1]);
			close(stderr_pipe[0]);
			close(stderr_pipe[1]);
			unlink(snapshot);
			return -ENOMEM;
		}
		memcpy(reads, sandbox.read_paths,
		       (size_t)sandbox.read_paths_count * sizeof(*reads));
		reads[sandbox.read_paths_count++] = snapshot;
		reads[sandbox.read_paths_count++] = (char *)runner;
		sandbox.read_paths = reads;
	}
	pid = fork();
	if (pid < 0) {
		int e = errno;
		close(stdin_pipe[0]);
		close(stdin_pipe[1]);
		close(stdout_pipe[0]);
		close(stdout_pipe[1]);
		close(stderr_pipe[0]);
		close(stderr_pipe[1]);
		unlink(snapshot);
		return -e;
	}
	if (pid == 0) {
		morph_signals_reset_child();
		if (sandbox_start_isolated_session() != 0) {
			_exit(126);
		}
		const char *workdir;

		close(stdin_pipe[1]);
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);
		dup2(stdin_pipe[0], STDIN_FILENO);
		dup2(stdout_pipe[1], STDOUT_FILENO);
		dup2(stderr_pipe[1], STDERR_FILENO);
		close(stdin_pipe[0]);
		close(stdout_pipe[1]);
		close(stderr_pipe[1]);
		rc = set_child_env(dt);
		if (rc < 0) {
			fprintf(stderr, "dynamic tool path context is unavailable\n");
			_exit(126);
		}
		workdir = tool_context_workdir_borrow(dt->tctx);
		if (chdir(workdir) != 0) {
			fprintf(stderr, "dynamic tool chdir(%s) failed: %s\n", workdir,
				strerror(errno));
			_exit(126);
		}
		if (sandbox_close_inherited_fds() != 0 ||
		    (!sandbox.read_all && sandbox_enter(&sandbox) != 0))
			_exit(126);
		execl(runner, runner, snapshot, (char *)NULL);
		execlp("morph-js-runner", "morph-js-runner", snapshot, (char *)NULL);
		_exit(127);
	}
	close(stdin_pipe[0]);
	close(stdout_pipe[1]);
	close(stderr_pipe[1]);
	{
		struct jsonrpc_request req;
		memset(&req, 0, sizeof(req));
		req.id = 1;
		req.method = "run";
		req.params_json = args_json ? args_json : "{}";
		request_str = jsonrpc_request_create(&req);
	}
	if (!request_str) {
		close(stdin_pipe[1]);
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);
		kill(pid, SIGKILL);
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
		unlink(snapshot);
		return tool_result_error_copy(result, "tool_failed",
					      "failed to build dynamic tool request");
	}
	morph_buf_t request MORPH_CLEANUP(morph_buf_cleanup) = {0};
	rc = morph_buf_init(&request, strlen(request_str) + 2);
	if (rc == 0) {
		rc = morph_buf_puts(&request, request_str);
	}
	if (rc == 0) {
		rc = morph_buf_putc(&request, '\n');
	}
	free(request_str);
	if (rc == 0) {
		rc = morph_buf_init(&out, 8192);
	}
	if (rc == 0) {
		rc = morph_buf_init(&err, 1024);
	}
	if (rc == 0) {
		int seconds = dt->cfg->default_timeout_seconds;
		int milliseconds = seconds <= 0		      ? 300000
				   : seconds > INT_MAX / 1000 ? INT_MAX
							      : seconds * 1000;
		rc = morph_child_exchange(
			pid, stdin_pipe[1], stdout_pipe[0], stderr_pipe[0],
			morph_buf_cstr_borrow(&request), request.len, milliseconds,
			output_limit,
			dynamic_tool_runner_cancelled, result, &out, &err, &status);
	} else {
		close(stdin_pipe[1]);
		kill(pid, SIGKILL);
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	}
	close(stdout_pipe[0]);
	close(stderr_pipe[0]);
	unlink(snapshot);
	morph_buf_cleanup(&request);

	if (err.len > 0)
		log_warn(result && result->runtime ? result->runtime->http.logger : NULL,
			 "dynamic tool %s stderr: %s", dt->name, morph_buf_cstr_borrow(&err));
	if (rc < 0) {
		morph_buf_cleanup(&out);
		morph_buf_cleanup(&err);
		return rc;
	}
	if (out.len > output_limit) {
		(void)tool_result_error_format(result, "output_limit_exceeded",
			"Dynamic tool output exceeds the configured %zu-byte capture limit. "
			"Return a smaller result or save large data to a file and return its path.",
			output_limit);
		return -EFBIG;
	}
	if (WIFSIGNALED(status)) {
		morph_buf_cleanup(&out);
		morph_buf_cleanup(&err);
		(void)tool_result_error_copy(result, "tool_failed",
					     "dynamic tool process terminated");
		return -EIO;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) != 0 && out.len == 0) {
		char msg[1024];

		snprintf(msg, sizeof(msg), "dynamic tool process exited with status %d%s%s",
			 WEXITSTATUS(status), err.len > 0 ? ": " : "",
			 err.len > 0 ? morph_buf_cstr_borrow(&err) : "");
		morph_buf_cleanup(&out);
		morph_buf_cleanup(&err);
		(void)tool_result_error_copy(result, "tool_failed", msg);
		return -EIO;
	}
	{
		struct jsonrpc_response jr = {0};
		char *raw = morph_buf_detach(&out);
		int prc;
		if (!raw) {
			morph_buf_cleanup(&err);
			return -ENOMEM;
		}
		prc = jsonrpc_response_copy(raw, &jr);
		free(raw);
		if (prc < 0) {
			morph_buf_cleanup(&err);
			(void)tool_result_error_copy(result, "tool_failed",
						     "invalid dynamic tool response");
			return prc;
		}
		if (jr.has_error) {
			char msg[1024];
			snprintf(msg, sizeof(msg), "dynamic tool error: %s",
				 jr.error_message ? jr.error_message : "unknown");
			jsonrpc_response_free(&jr);
			morph_buf_cleanup(&err);
			(void)tool_result_error_copy(result, "tool_failed", msg);
			return -EIO;
		}
		rc = tool_result_success_json_text_copy(result,
							jr.result_json ? jr.result_json : "null");
		jsonrpc_response_free(&jr);
	}
	morph_buf_cleanup(&err);
	return rc;
}

int dynamic_tool_runner_check_js_source(const char *source_path, char **error_out)
{
	pid_t pid;
	int output_pipe[2] = {-1, -1};
	int status = 0;
	char runner_storage[PATH_MAX];
	const char *runner = runner_path(runner_storage);

	if (!source_path)
		return -EINVAL;
	if (error_out && *error_out)
		return -EALREADY;
	int pipe_rc = morph_io_pipe(output_pipe);
	if (pipe_rc != 0)
		return pipe_rc;
	pid = fork();
	if (pid < 0) {
		int rc = -errno;
		close(output_pipe[0]);
		close(output_pipe[1]);
		return rc;
	}
	if (pid == 0) {
		morph_signals_reset_child();
		int first_errno;
		close(output_pipe[0]);
		dup2(output_pipe[1], STDOUT_FILENO);
		dup2(output_pipe[1], STDERR_FILENO);
		close(output_pipe[1]);
		if (sandbox_start_isolated_session() != 0 || sandbox_close_inherited_fds() != 0) {
			_exit(126);
		}
		execl(runner, runner, "--check", source_path, (char *)NULL);
		first_errno = errno;
		execlp("morph-js-runner", "morph-js-runner", "--check", source_path, (char *)NULL);
		fprintf(stderr,
			"failed to exec morph-js-runner. runner=%s "
			"source=%s direct_errno=%s fallback_errno=%s\n",
			runner ? runner : "(null)", source_path ? source_path : "(null)",
			strerror(first_errno), strerror(errno));
		_exit(127);
	}
	close(output_pipe[1]);
	morph_buf_t output = {0}, errors = {0};
	int input_fd = open("/dev/null", O_WRONLY);
	int rc = input_fd < 0 ? -errno : morph_buf_init(&output, 512);
	if (rc == 0) {
		rc = morph_buf_init(&errors, 16);
	}
	if (rc == 0) {
		rc = morph_child_exchange(pid, input_fd, output_pipe[0], -1, "", 0, 30000, 8192,
					  NULL, NULL, &output, &errors, &status);
	} else {
		if (input_fd >= 0) {
			close(input_fd);
		}
		kill(pid, SIGKILL);
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	}
	close(output_pipe[0]);
	morph_buf_cleanup(&errors);
	if (error_out != NULL && output.len != 0) {
		*error_out = morph_buf_detach(&output);
	}
	morph_buf_cleanup(&output);
	if (rc != 0) {
		return rc;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		if (error_out && !*error_out) {
			morph_buf_t err MORPH_CLEANUP(morph_buf_cleanup) = {0};
			if (morph_buf_init(&err, 256) == 0) {
				if (WIFEXITED(status)) {
					(void)morph_buf_printf(&err,
							       "morph-js-runner --check failed "
							       "without output. runner=%s "
							       "source=%s exit_status=%d",
							       runner,
							       source_path ? source_path : "(null)",
							       WEXITSTATUS(status));
				} else if (WIFSIGNALED(status)) {
					(void)morph_buf_printf(&err,
							       "morph-js-runner --check was "
							       "terminated by signal %d. "
							       "runner=%s source=%s",
							       WTERMSIG(status), runner,
							       source_path ? source_path
									   : "(null)");
				} else {
					(void)morph_buf_printf(&err,
							       "morph-js-runner --check failed "
							       "without output. runner=%s "
							       "source=%s status=%d",
							       runner,
							       source_path ? source_path : "(null)",
							       status);
				}
				*error_out = morph_buf_detach(&err);
			}
		}
		return -EINVAL;
	}
	return 0;
}

int dynamic_tool_runner_check_unpublished_source(const char *source, char **error_out)
{
	char path[] = "/tmp/morph-tool-check-XXXXXX";
	int fd;
	int rc;

	if (!source || !error_out)
		return -EINVAL;
	if (*error_out)
		return -EALREADY;
	fd = mkstemp(path);
	if (fd < 0) {
		return -errno;
	}
	rc = close(fd) == 0 ? 0 : -errno;
	if (rc == 0) {
		rc = file_write_all(path, source, strlen(source));
	}
	if (rc == 0) {
		rc = dynamic_tool_runner_check_js_source(path, error_out);
	}
	if (unlink(path) != 0 && rc == 0) {
		rc = -errno;
	}
	return rc;
}
