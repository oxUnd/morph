#include "dynamic_tool_runner.h"
#include "nb/signal.h"
#include "nb/process.h"
#include "sandbox/sandbox.h"
#include "sandbox/permission.h"
#include "agent/tool_runtime.h"
#include "ipc/jsonrpc.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "runtime/resources.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/str.h"
#include "nb/log.h"
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
static int append_list(nb_buf *buf, const char *value, int *first);
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

static int append_list(nb_buf *buf, const char *value, int *first)
{
	int rc;

	if (!value || !*value)
		return 0;
	if (!*first) {
		rc = (nb_buf_append_char(buf, ',') ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
	}
	*first = 0;
	return (nb_buf_append_cstr(buf, value) ? 0 : NB_ERR_NOMEM);
}

static char *join_caps(char caps[][DYNAMIC_TOOL_CAP_LEN_MAX], int count)
{
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	int first = 1;

	if ((nb_buf_init(&buf), nb_buf_reserve(&buf, 128) ? 0 : NB_ERR_NOMEM) != 0)
		return NULL;
	for (int i = 0; i < count; i++) {
		if (append_list(&buf, caps[i], &first) != 0) {
			nb_buf_cleanup(&buf);
			return NULL;
		}
	}
	return (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
}

static char *join_allow(const char values[][DYNAMIC_TOOL_ALLOW_LEN_MAX], int count)
{
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	int first = 1;

	if ((nb_buf_init(&buf), nb_buf_reserve(&buf, 128) ? 0 : NB_ERR_NOMEM) != 0)
		return NULL;
	for (int i = 0; i < count; i++) {
		if (append_list(&buf, values[i], &first) != 0) {
			nb_buf_cleanup(&buf);
			return NULL;
		}
	}
	return (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
}

static char *join_path_allow(const char values[][DYNAMIC_TOOL_ALLOW_LEN_MAX], int count,
			     const char *root)
{
	nb_buf candidate_path NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf resolved NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf buf NB_CLEANUP(nb_buf_destroy) = {0};
	int first = 1;

	if (!nb_buf_reserve(&buf, 128))
		return NULL;
	for (int i = 0; i < count; i++) {
		int rc;

		if (!values[i][0])
			continue;
		if (strcmp(values[i], "*") == 0) {
			rc = append_list(&buf, values[i], &first);
		} else {
			const char *candidate = values[i];

			if (!nb_path_is_absolute(candidate)) {
				if (nb_path_join(&candidate_path, root, candidate) != NB_OK)
					return NULL;
				candidate = nb_buf_cstr(&candidate_path);
			}
			if (nb_path_expand(&resolved, candidate, NULL) != NB_OK ||
			    nb_path_canonical_missing(&resolved, nb_buf_cstr(&resolved), NULL) != NB_OK)
				return NULL;
			rc = append_list(&buf, nb_buf_cstr(&resolved), &first);
		}
		if (rc != 0)
			return NULL;
	}
	return nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL;
}

static int effective_caps(struct dynamic_tool *dt,
			  char out[DYNAMIC_TOOL_CAP_MAX][DYNAMIC_TOOL_CAP_LEN_MAX], int *out_count)
{
	const struct config_dynamic_tool_profile *profile;
	int count = 0;

	if (!dt || !dt->cfg || !out || !out_count)
		return NB_ERR_INVAL;
	profile = active_profile(dt->cfg);
	if (!profile)
		return NB_ERR_INVAL;
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
		return NB_ERR_INVAL;
	workdir = tool_context_workdir_borrow(dt->tctx);
	output_dir = tool_context_output_dir_borrow(dt->tctx);
	if (!workdir || !*workdir || !output_dir || !*output_dir)
		return NB_ERR_INVAL;
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
		return NB_ERR_NOMEM;
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
		rc = nb_status_from_errno(errno, NULL);
	free(cap_str);
	free(read_str);
	free(write_str);
	free(command_str);
	free(network_str);
	return rc;
}

struct dynamic_child_context {
	struct dynamic_tool *tool;
	struct sandbox_config *sandbox;
};

static nb_status dynamic_prepare_child(void *opaque)
{
	struct dynamic_child_context *context = opaque;
	/* The child uses only its private configuration, without parent locks. */
	context->sandbox->logger = NULL;
	int rc = set_child_env(context->tool);
	if (rc == NB_OK && !context->sandbox->read_all)
		rc = sandbox_enter(context->sandbox);
	return (nb_status)rc;
}

int dynamic_tool_runner_execute(const char *args_json, struct tool_result *result, void *user_data)
{
	struct dynamic_tool *dt = user_data;
	char *request_str = NULL;
	nb_buf out NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf err NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc;

	int status = 0;

	if (!dt || !result)
		return NB_ERR_INVAL;
	size_t output_limit = dt->cfg->default_max_output_bytes > 0
		? (size_t)dt->cfg->default_max_output_bytes : DYN_TOOL_OUTPUT_LIMIT;
	nb_buf snapshot_path NB_CLEANUP(nb_buf_cleanup) = {0};
	rc = nb_file_temp_in(&snapshot_path, "/tmp", NULL, NULL);
	if (rc != NB_OK)
		return rc;
	char *snapshot = snapshot_path.ptr;
	rc = dt->source != NULL ? nb_file_write_all(snapshot, dt->source, strlen(dt->source), NULL)
				: NB_ERR_NOT_FOUND;
	if (rc != 0) {
		unlink(snapshot);
		return rc;
	}

	struct sandbox_config sandbox NB_CLEANUP(sandbox_config_cleanup) = {0};
	nb_vec read_views NB_CLEANUP(nb_vec_cleanup) = {0};
	char runner_storage[PATH_MAX];
	const char *runner = runner_path(runner_storage);

	rc = sandbox_permission_prepare(dt->tctx, dt->name, NULL, &sandbox);
	if (rc != 0) {

		unlink(snapshot);
		return rc;
	}
	if (!sandbox.read_all) {
		rc = (nb_vec_init(&read_views, sizeof(char *)),
		      nb_vec_reserve(&read_views, (size_t)sandbox.read_paths_count + 2)
			      ? 0
			      : NB_ERR_NOMEM);
		char **reads =
			rc == 0 ? nb_vec_push_n(&read_views, (size_t)sandbox.read_paths_count + 2)
				: NULL;

		if (!reads) {

			unlink(snapshot);
			return NB_ERR_NOMEM;
		}
		memcpy(reads, sandbox.read_paths,
		       (size_t)sandbox.read_paths_count * sizeof(*reads));
		reads[sandbox.read_paths_count++] = snapshot;
		reads[sandbox.read_paths_count++] = (char *)runner;
		sandbox.read_paths = reads;
	}
	struct dynamic_child_context context = {.tool = dt, .sandbox = &sandbox};
	const char *argv[] = {nb_file_exists(runner) ? runner : "morph-js-runner", snapshot, NULL};
	nb_process_options options = {
		.argv = argv,
		.cwd = tool_context_workdir_borrow(dt->tctx),
		.search_path = true,
		.close_fds = true,
		.new_session = true,
		.prepare = dynamic_prepare_child,
		.prepare_ctx = &context,
	};
	nb_process *child NB_CLEANUP(nb_process_cleanup) = NULL;
	rc = nb_process_spawn(&options, &child, NULL);
	if (rc != NB_OK) {
		unlink(snapshot);
		return rc;
	}
	{
		struct jsonrpc_request req;
		memset(&req, 0, sizeof(req));
		req.id = 1;
		req.method = "run";
		req.params_json = args_json ? args_json : "{}";
		request_str = jsonrpc_request_create(&req);
	}
	if (!request_str) {

		unlink(snapshot);
		return tool_result_error_copy(result, "tool_failed",
					      "failed to build dynamic tool request");
	}
	nb_buf request NB_CLEANUP(nb_buf_cleanup) = {0};
	rc = (nb_buf_init(&request),
	      nb_buf_reserve(&request, strlen(request_str) + 2) ? 0 : NB_ERR_NOMEM);
	if (rc == 0) {
		rc = (nb_buf_append_cstr(&request, request_str) ? 0 : NB_ERR_NOMEM);
	}
	if (rc == 0) {
		rc = (nb_buf_append_char(&request, '\n') ? 0 : NB_ERR_NOMEM);
	}
	free(request_str);
	if (rc == 0) {
		rc = (nb_buf_init(&out), nb_buf_reserve(&out, 8192) ? 0 : NB_ERR_NOMEM);
	}
	if (rc == 0) {
		rc = (nb_buf_init(&err), nb_buf_reserve(&err, 1024) ? 0 : NB_ERR_NOMEM);
	}
	bool truncated = false;
	if (rc == NB_OK) {
		int seconds = dt->cfg->default_timeout_seconds;
		int milliseconds = seconds <= 0		      ? 300000
				   : seconds > INT_MAX / 1000 ? INT_MAX
							      : seconds * 1000;
		nb_process_exchange_options exchange = {
			.timeout_ms = milliseconds,
			.max_output = output_limit,
			.cancelled = dynamic_tool_runner_cancelled,
			.cancel_ctx = result,
			.stdout_truncated = &truncated,
		};
		rc = nb_process_exchange(child, nb_buf_cstr(&request), nb_buf_len(&request),
					 &exchange, &out, &err, &status, NULL);
	}
	unlink(snapshot);
	nb_buf_cleanup(&request);

	if (err.len > 0)
		nb_log_write(result && result->runtime ? result->runtime->http.logger : NULL,
			     NB_LOG_WARN, "dynamic tool %s stderr: %s", dt->name,
			     nb_buf_cstr(&err));
	if (rc < 0) {
		nb_buf_cleanup(&out);
		nb_buf_cleanup(&err);
		return rc;
	}
	if (truncated) {
		(void)tool_result_error_format(result, "output_limit_exceeded",
			"Dynamic tool output exceeds the configured %zu-byte capture limit. "
			"Return a smaller result or save large data to a file and return its path.",
			output_limit);
		return NB_ERR_TOO_BIG;
	}
	if (nb_process_exit_signal(child) != 0) {
		nb_buf_cleanup(&out);
		nb_buf_cleanup(&err);
		(void)tool_result_error_copy(result, "tool_failed",
					     "dynamic tool process terminated");
		return NB_ERR_IO;
	}
	if (nb_process_exit_signal(child) == 0 && status != 0 && out.len == 0) {
		char msg[1024];

		snprintf(msg, sizeof(msg), "dynamic tool process exited with status %d%s%s", status,
			 err.len > 0 ? ": " : "", err.len > 0 ? nb_buf_cstr(&err) : "");
		nb_buf_cleanup(&out);
		nb_buf_cleanup(&err);
		(void)tool_result_error_copy(result, "tool_failed", msg);
		return NB_ERR_IO;
	}
	{
		struct jsonrpc_response jr = {0};
		char *raw = (nb_buf_ok(&out) ? strdup(nb_buf_cstr(&out)) : NULL);
		int prc;
		if (!raw) {
			nb_buf_cleanup(&err);
			return NB_ERR_NOMEM;
		}
		prc = jsonrpc_response_copy(raw, &jr);
		free(raw);
		if (prc < 0) {
			nb_buf_cleanup(&err);
			(void)tool_result_error_copy(result, "tool_failed",
						     "invalid dynamic tool response");
			return prc;
		}
		if (jr.has_error) {
			char msg[1024];
			snprintf(msg, sizeof(msg), "dynamic tool error: %s",
				 jr.error_message ? jr.error_message : "unknown");
			jsonrpc_response_free(&jr);
			nb_buf_cleanup(&err);
			(void)tool_result_error_copy(result, "tool_failed", msg);
			return NB_ERR_IO;
		}
		rc = tool_result_success_json_text_copy(result,
							jr.result_json ? jr.result_json : "null");
		jsonrpc_response_free(&jr);
	}
	nb_buf_cleanup(&err);
	return rc;
}

int dynamic_tool_runner_check_js_source(const char *source_path, char **error_out)
{
	if (!source_path || (error_out && *error_out))
		return NB_ERR_INVAL;
	char runner_storage[PATH_MAX];
	const char *runner = runner_path(runner_storage);
	const char *argv[] = {nb_file_exists(runner) ? runner : "morph-js-runner", "--check",
			      source_path, NULL};
	nb_process_options options = {
		.argv = argv,
		.search_path = true,
		.close_fds = true,
		.new_session = true,
		.merge_stderr = true,
	};
	nb_process *child NB_CLEANUP(nb_process_cleanup) = NULL;
	int rc = nb_process_spawn(&options, &child, NULL);
	if (rc != NB_OK)
		return rc;
	nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf errors NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_process_exchange_options exchange = {.timeout_ms = 30000, .max_output = 8192};
	int code = 0;
	rc = nb_process_exchange(child, "", 0, &exchange, &output, &errors, &code, NULL);
	if (error_out && nb_buf_len(&output)) {
		*error_out = strdup(nb_buf_cstr(&output));
		if (!*error_out)
			return NB_ERR_NOMEM;
	}
	if (rc != NB_OK)
		return rc;
	if (code != 0) {
		if (error_out && !*error_out) {
			nb_buf detail NB_CLEANUP(nb_buf_cleanup) = {0};
			if (!nb_buf_printf(&detail,
					   "morph-js-runner --check failed without output. "
					   "runner=%s source=%s exit_status=%d",
					   runner, source_path, code))
				return NB_ERR_NOMEM;
			*error_out = strdup(nb_buf_cstr(&detail));
			if (!*error_out)
				return NB_ERR_NOMEM;
		}
		return NB_ERR_INVAL;
	}
	return NB_OK;
}

int dynamic_tool_runner_check_unpublished_source(const char *source, char **error_out)
{
	nb_buf temporary NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc;

	if (!source || !error_out)
		return NB_ERR_INVAL;
	if (*error_out)
		return NB_ERR_STATE;
	rc = nb_file_temp_in(&temporary, "/tmp", NULL, NULL);
	if (rc != NB_OK)
		return rc;
	const char *path = nb_buf_cstr(&temporary);
	if (rc == 0) {
		rc = nb_file_write_all(path, source, strlen(source), NULL);
	}
	if (rc == 0) {
		rc = dynamic_tool_runner_check_js_source(path, error_out);
	}
	if (unlink(path) != 0 && rc == 0) {
		rc = nb_status_from_errno(errno, NULL);
	}
	return rc;
}
