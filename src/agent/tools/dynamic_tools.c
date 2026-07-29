#include "dynamic_tools.h"
#include "ipc/jsonrpc.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/file.h"
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

#ifndef MORPH_JS_RUNNER_PATH
#define MORPH_JS_RUNNER_PATH "morph-js-runner"
#endif

#define DYN_TOOL_META_FILE "tool.json"
#define DYN_TOOL_SOURCE_FILE "tool.js"
#define DYN_TOOL_HISTORY_DIR ".history"
#define DYN_TOOL_CHECKPOINT_META_FILE "checkpoint.json"
#define DYN_TOOL_OUTPUT_LIMIT (1024 * 1024)
#define DYN_TOOL_HISTORY_LIMIT 20

struct dynamic_tool {
	char name[TOOL_NAME_MAX];
	char description[TOOL_DESC_MAX];
	char input_schema[TOOL_SCHEMA_MAX];
	char output_schema[TOOL_SCHEMA_MAX];
	char source_path[PATH_MAX];
	char dir[PATH_MAX];
	const struct config_dynamic_tools *cfg;
	struct tool_context *tctx;
	enum tool_origin origin;
};

struct dynamic_tools_context {
	struct tool_registry *reg;
	struct tool_context *tctx;
	const struct config_dynamic_tools *cfg;
	char session_id[128];
};

static int dynamic_tools_load_session(struct tool_registry *reg,
				      struct tool_context *tctx,
				      const struct config_dynamic_tools *cfg,
				      const char *session_id);
static int load_tool_from_dir(struct tool_registry *reg,
			      struct tool_context *tctx,
			      const struct config_dynamic_tools *cfg,
			      const char *dir,
			      enum tool_origin origin);
static int remove_tree(const char *path);
static int copy_file(const char *src, const char *dst);

struct tool_checkpoint {
	char id[16];
	char name[TOOL_NAME_MAX];
	char before_state[16];
	char origin[32];
	char old_hash[32];
	char new_hash[32];
	long created_at;
};

static const char *runner_path(void)
{
	const char *path = getenv("MORPH_JS_RUNNER_PATH");
	if (path && *path)
		return path;
#ifdef __ANDROID__
	{
		static char android_runner[PATH_MAX];
		Dl_info info;
		const char *slash;
		size_t dir_len;

		if (android_runner[0])
			return android_runner;
		memset(&info, 0, sizeof(info));
		if (dladdr((void *)(uintptr_t)runner_path, &info) != 0 &&
		    info.dli_fname && *info.dli_fname) {
			slash = strrchr(info.dli_fname, '/');
			if (slash) {
				dir_len = (size_t)(slash - info.dli_fname);
				if (dir_len > 0 && dir_len < sizeof(android_runner)) {
					memcpy(android_runner, info.dli_fname,
					       dir_len);
					android_runner[dir_len] = '\0';
					if (snprintf(android_runner + dir_len,
						sizeof(android_runner) - dir_len,
						"/libmorph-js-runner.so") <
					    (int)(sizeof(android_runner) -
					    dir_len)) {
						return android_runner;
					}
					android_runner[0] = '\0';
				}
			}
		}
	}
#endif
	return MORPH_JS_RUNNER_PATH;
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

static int valid_tool_name(const char *name)
{
	if (!name || !*name)
		return 0;
	for (const char *p = name; *p; p++) {
		if ((*p >= 'a' && *p <= 'z') ||
		    (*p >= '0' && *p <= '9') || *p == '_')
			continue;
		return 0;
	}
	return 1;
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
	morph_buf_t buf;
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

static char *join_allow(const char values[][DYNAMIC_TOOL_ALLOW_LEN_MAX],
			int count)
{
	morph_buf_t buf;
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

static char *join_path_allow(
	const char values[][DYNAMIC_TOOL_ALLOW_LEN_MAX],
	int count, const char *root)
{
	morph_buf_t buf;
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
			resolved = file_resolve_path(candidate);
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
			  char out[DYNAMIC_TOOL_CAP_MAX][DYNAMIC_TOOL_CAP_LEN_MAX],
			  int *out_count)
{
	const struct config_dynamic_tool_profile *profile;
	int count = 0;

	if (!dt || !dt->cfg || !out || !out_count)
		return -EINVAL;
	profile = active_profile(dt->cfg);
	if (!profile)
		return -EINVAL;
	for (int i = 0; i < profile->default_capabilities_count &&
	     count < DYNAMIC_TOOL_CAP_MAX; i++) {
		strncpy(out[count], profile->default_capabilities[i],
			DYNAMIC_TOOL_CAP_LEN_MAX - 1);
		count++;
	}
	*out_count = count;
	return 0;
}

static int read_fd_limited(int fd, morph_buf_t *buf, int limit)
{
	char tmp[BUFSIZ];

	while (1) {
		ssize_t n = read(fd, tmp, sizeof(tmp));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				return 0;
			MORPH_RETURN_ERRNO();
		}
		if (n == 0)
			return 1;
		if ((int)buf->len < limit) {
			size_t keep = (size_t)n;
			if (buf->len + keep > (size_t)limit)
				keep = (size_t)limit - buf->len;
			if (keep > 0 && morph_buf_append(buf, tmp, keep) != 0)
				return -ENOMEM;
		}
	}
}

static int read_pipes(int out_fd, int err_fd, morph_buf_t *out,
		      morph_buf_t *err, int timeout_seconds, int output_limit,
		      int *timed_out)
{
	int out_open = 1;
	int err_open = 1;
	int max_fd = out_fd > err_fd ? out_fd : err_fd;
	time_t deadline = time(NULL) + timeout_seconds;

	fcntl(out_fd, F_SETFL, O_NONBLOCK);
	fcntl(err_fd, F_SETFL, O_NONBLOCK);
	while (out_open || err_open) {
		fd_set rfds;
		time_t now;
		struct timeval tv;
		int rc;

		now = time(NULL);
		if (timeout_seconds > 0 && now >= deadline) {
			*timed_out = 1;
			return 0;
		}
		FD_ZERO(&rfds);
		if (out_open)
			FD_SET(out_fd, &rfds);
		if (err_open)
			FD_SET(err_fd, &rfds);
		tv.tv_sec = timeout_seconds > 0 ? deadline - now : 30;
		tv.tv_usec = 0;
		rc = select(max_fd + 1, &rfds, NULL, NULL, &tv);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			MORPH_RETURN_ERRNO();
		}
		if (rc == 0) {
			if (timeout_seconds > 0)
				*timed_out = 1;
			return 0;
		}
		if (out_open && FD_ISSET(out_fd, &rfds)) {
			rc = read_fd_limited(out_fd, out, output_limit);
			if (rc < 0)
				return rc;
			if (rc > 0)
				out_open = 0;
		}
		if (err_open && FD_ISSET(err_fd, &rfds)) {
			rc = read_fd_limited(err_fd, err, output_limit);
			if (rc < 0)
				return rc;
			if (rc > 0)
				err_open = 0;
		}
	}
	return 0;
}

static void dynamic_tool_destroy(void *user_data)
{
	free(user_data);
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
	int rc;

	if (!dt || !dt->tctx)
		return -EINVAL;
	workdir = tool_context_workdir(dt->tctx);
	output_dir = tool_context_output_dir(dt->tctx);
	if (!workdir || !*workdir || !output_dir || !*output_dir)
		return -EINVAL;
	profile = active_profile(dt->cfg);
	rc = effective_caps(dt, caps, &caps_count);
	if (rc < 0)
		return rc;
	cap_str = join_caps(caps, caps_count);
	read_str = join_path_allow(profile->allowed_read_paths,
				   profile->allowed_read_paths_count,
				   workdir);
	write_str = join_path_allow(profile->allowed_write_paths,
				    profile->allowed_write_paths_count,
				    output_dir);
	command_str = join_allow(profile->allowed_commands,
				 profile->allowed_commands_count);
	network_str = join_allow(profile->allowed_network,
				 profile->allowed_network_count);
	if (!cap_str || !read_str || !write_str || !command_str ||
	    !network_str) {
		free(cap_str);
		free(read_str);
		free(write_str);
		free(command_str);
		free(network_str);
		return -ENOMEM;
	}
	snprintf(timeout_buf, sizeof(timeout_buf), "%d",
		 dt->cfg->default_timeout_seconds > 0
		 ? dt->cfg->default_timeout_seconds : 30);
	rc = 0;
	if (setenv("MORPH_DYNAMIC_CAPS", cap_str, 1) != 0 ||
	    setenv("MORPH_DYNAMIC_ALLOWED_READ", read_str, 1) != 0 ||
	    setenv("MORPH_DYNAMIC_ALLOWED_WRITE", write_str, 1) != 0 ||
	    setenv("MORPH_DYNAMIC_ALLOWED_COMMANDS", command_str, 1) != 0 ||
	    setenv("MORPH_DYNAMIC_ALLOWED_NETWORK", network_str, 1) != 0 ||
	    setenv("MORPH_DYNAMIC_TIMEOUT", timeout_buf, 1) != 0 ||
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

static int dynamic_tool_exec(const char *args_json, struct tool_result *result,
			     void *user_data)
{
	struct dynamic_tool *dt = user_data;
	int stdin_pipe[2];
	int stdout_pipe[2];
	int stderr_pipe[2];
	pid_t pid;
	char *request_str = NULL;
	morph_buf_t out;
	morph_buf_t err;
	int rc;
	int timed_out = 0;
	int status = 0;

	if (!dt || !result)
		return -EINVAL;
	if (pipe(stdin_pipe) < 0)
		MORPH_RETURN_ERRNO();
	if (pipe(stdout_pipe) < 0) {
		int e = errno;
		close(stdin_pipe[0]);
		close(stdin_pipe[1]);
		MORPH_RETURN(-e);
	}
	if (pipe(stderr_pipe) < 0) {
		int e = errno;
		close(stdin_pipe[0]);
		close(stdin_pipe[1]);
		close(stdout_pipe[0]);
		close(stdout_pipe[1]);
		MORPH_RETURN(-e);
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
		MORPH_RETURN(-e);
	}
	if (pid == 0) {
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
			fprintf(stderr,
				"dynamic tool path context is unavailable\n");
			_exit(126);
		}
		workdir = tool_context_workdir(dt->tctx);
		if (chdir(workdir) != 0) {
			fprintf(stderr, "dynamic tool chdir(%s) failed: %s\n",
				workdir, strerror(errno));
			_exit(126);
		}
		const char *runner = runner_path();
		execl(runner, runner,
		      dt->source_path, (char *)NULL);
		execlp("morph-js-runner", "morph-js-runner",
		       dt->source_path, (char *)NULL);
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
		request_str = jsonrpc_build_request(&req);
	}
	if (!request_str) {
		close(stdin_pipe[1]);
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);
		waitpid(pid, NULL, 0);
		return tool_result_error(result, "tool_failed",
					      "failed to build dynamic tool request");
	}
	{
		size_t len = strlen(request_str);
		size_t written = 0;
		while (written < len) {
			ssize_t n = write(stdin_pipe[1], request_str + written,
					  len - written);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			written += (size_t)n;
		}
		write(stdin_pipe[1], "\n", 1);
	}
	free(request_str);
	close(stdin_pipe[1]);
	if (morph_buf_init(&out, 8192) != 0 ||
	    morph_buf_init(&err, 1024) != 0) {
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);
		waitpid(pid, NULL, 0);
		return -ENOMEM;
	}
	rc = read_pipes(stdout_pipe[0], stderr_pipe[0], &out, &err,
			dt->cfg->default_timeout_seconds,
			dt->cfg->default_max_output_bytes > 0
				? dt->cfg->default_max_output_bytes
				: DYN_TOOL_OUTPUT_LIMIT,
			&timed_out);
	close(stdout_pipe[0]);
	close(stderr_pipe[0]);
	if (timed_out) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		morph_buf_cleanup(&out);
		morph_buf_cleanup(&err);
		(void)tool_result_error(result, "tool_failed", "dynamic tool timed out");
		return -ETIMEDOUT;
	}
	waitpid(pid, &status, 0);
	if (err.len > 0)
		log_warn("dynamic tool %s stderr: %s", dt->name,
			 morph_buf_cstr(&err));
	if (rc < 0) {
		morph_buf_cleanup(&out);
		morph_buf_cleanup(&err);
		return rc;
	}
	if (WIFSIGNALED(status)) {
		morph_buf_cleanup(&out);
		morph_buf_cleanup(&err);
		(void)tool_result_error(result, "tool_failed",
					     "dynamic tool process terminated");
		return -EIO;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) != 0 && out.len == 0) {
		char msg[1024];

		snprintf(msg, sizeof(msg),
			 "dynamic tool process exited with status %d%s%s",
			 WEXITSTATUS(status),
			 err.len > 0 ? ": " : "",
			 err.len > 0 ? morph_buf_cstr(&err) : "");
		morph_buf_cleanup(&out);
		morph_buf_cleanup(&err);
		(void)tool_result_error(result, "tool_failed", msg);
		return -EIO;
	}
	{
		struct jsonrpc_response jr;
		char *raw = morph_buf_detach(&out);
		int prc;
		if (!raw) {
			morph_buf_cleanup(&err);
			return -ENOMEM;
		}
		prc = jsonrpc_parse_response(raw, &jr);
		free(raw);
		if (prc < 0) {
			morph_buf_cleanup(&err);
			(void)tool_result_error(result, "tool_failed",
						     "invalid dynamic tool response");
			return prc;
		}
		if (jr.has_error) {
			char msg[1024];
			snprintf(msg, sizeof(msg), "dynamic tool error: %s",
				 jr.error_message ? jr.error_message : "unknown");
			jsonrpc_response_free(&jr);
			morph_buf_cleanup(&err);
			(void)tool_result_error(result, "tool_failed", msg);
			return -EIO;
		}
		rc = tool_result_success_json_text(result,
					   jr.result_json ? jr.result_json :
					   strdup("null"));
		jr.result_json = NULL;
		jsonrpc_response_free(&jr);
	}
	morph_buf_cleanup(&err);
	return rc;
}

static int register_dynamic_tool(struct tool_registry *reg,
				 struct dynamic_tool *dt)
{
	struct tool_entry *existing;
	int rc;

	if (!reg || !dt)
		return -EINVAL;
	existing = tool_lookup(reg, dt->name);
	if (existing) {
		if (!(existing->flags & TOOL_FLAG_DYNAMIC))
			return -EEXIST;
		if (existing->user_data && existing->user_data_destroy)
			existing->user_data_destroy(existing->user_data);
		memset(&existing->desc, 0, sizeof(existing->desc));
		strncpy(existing->desc.name, dt->name,
			sizeof(existing->desc.name) - 1);
			strncpy(existing->desc.description, dt->description,
				sizeof(existing->desc.description) - 1);
			strncpy(existing->desc.input_schema, dt->input_schema,
				sizeof(existing->desc.input_schema) - 1);
			strncpy(existing->desc.output_schema, dt->output_schema,
				sizeof(existing->desc.output_schema) - 1);
		existing->exec = dynamic_tool_exec;
		existing->user_data = dt;
		existing->user_data_destroy = dynamic_tool_destroy;
		existing->origin = dt->origin;
		existing->timeout_seconds =
			dt->cfg->default_timeout_seconds > 0
			? dt->cfg->default_timeout_seconds
			: tool_context_default_timeout(dt->tctx);
		existing->flags |= TOOL_FLAG_DYNAMIC;
		log_dbg("dynamic tool replaced: %s", dt->name);
		return 1;
	}
		struct tool_spec spec = {
			.origin = dt->origin,
			.name = dt->name,
			.description = dt->description,
			.input_schema = dt->input_schema,
			.output_schema = dt->output_schema,
			.exec = dynamic_tool_exec,
			.user_data = dt,
			.user_data_destroy = dynamic_tool_destroy,
			.flags = TOOL_FLAG_DYNAMIC,
			.timeout_seconds = dt->cfg->default_timeout_seconds > 0
				? dt->cfg->default_timeout_seconds
				: tool_context_default_timeout(dt->tctx),
		};
		rc = tool_register(reg, &spec);
	if (rc < 0)
		return rc;
	{
		struct tool_entry *entry = tool_lookup(reg, dt->name);
		if (entry) {
			entry->flags |= TOOL_FLAG_DYNAMIC;
			entry->timeout_seconds =
				dt->cfg->default_timeout_seconds > 0
				? dt->cfg->default_timeout_seconds
				: tool_context_default_timeout(dt->tctx);
		}
	}
	return 0;
}

static char *schema_to_string(cJSON *item)
{
	if (!item)
		return strdup("{}");
	if (cJSON_IsString(item) && item->valuestring)
		return strdup(item->valuestring);
	return cJSON_PrintUnformatted(item);
}

static const char *json_type_name(cJSON *item)
{
	if (!item)
		return "missing";
	if (cJSON_IsString(item))
		return "string";
	if (cJSON_IsObject(item))
		return "object";
	if (cJSON_IsArray(item))
		return "array";
	if (cJSON_IsNumber(item))
		return "number";
	if (cJSON_IsBool(item))
		return "boolean";
	if (cJSON_IsNull(item))
		return "null";
	return "unknown";
}

static int append_tool_create_arg_error(morph_buf_t *buf,
					const char *field,
					const char *expected,
					cJSON *actual)
{
	return morph_buf_printf(buf, "- %s: expected %s, got %s\n",
				field, expected, json_type_name(actual));
}

static const char *forbidden_source_token(const char *source)
{
	static const char *tokens[] = {
		"require(",
		"require (",
		"Buffer",
		"process",
		"child_process",
		"node:",
		"__dirname",
		"__filename",
		NULL
	};

	if (!source)
		return NULL;
	for (int i = 0; tokens[i]; i++) {
		if (strstr(source, tokens[i]))
			return tokens[i];
	}
	return NULL;
}

static int validate_tool_create_args(cJSON *root, cJSON *name_item,
				     cJSON *source_item,
				     morph_buf_t *err)
{
	const char *name;
	const char *token;
	int rc;

	if (!root || !cJSON_IsObject(root)) {
		return morph_buf_puts(err,
				"tool_create arguments must be a JSON object with "
				"name, source_js, optional description, and "
				"input_schema/output_schema.\n");
	}
	rc = 0;
	if (!cJSON_IsString(name_item) || !name_item->valuestring) {
		rc = append_tool_create_arg_error(err, "name",
			"non-empty string matching ^[a-z0-9_]+$", name_item);
	} else if (!valid_tool_name(name_item->valuestring)) {
		rc = morph_buf_printf(err,
			"- name: invalid value \"%s\"; use only lowercase "
			"letters, digits, and underscore\n",
			name_item->valuestring);
	}
	name = cJSON_IsString(name_item) ? name_item->valuestring : "";
	(void)name;
	if (rc != 0)
		return rc;
	if (!cJSON_IsString(source_item) || !source_item->valuestring) {
		return append_tool_create_arg_error(err, "source_js",
			"JavaScript source string defining global run(args)",
			source_item);
	}
	if (!cJSON_GetObjectItem(root, "input_schema")) {
		return append_tool_create_arg_error(err, "input_schema",
			"JSON Schema object for input arguments", NULL);
	}
	if (!cJSON_GetObjectItem(root, "output_schema")) {
		return append_tool_create_arg_error(err, "output_schema",
			"JSON Schema object for result data", NULL);
	}
	token = forbidden_source_token(source_item->valuestring);
	if (token) {
		return morph_buf_printf(err,
			"- source_js: forbidden token \"%s\"; dynamic tools "
			"run in QuickJS, not Node.js. Use morph.image, "
			"morph.canvas, morph.fs, morph.fetch, and morph.env "
			"instead.\n",
			token);
	}
	return 0;
}

static char *format_check_error(const char *source_path, const char *output)
{
	morph_buf_t buf;

	if (morph_buf_init(&buf, 1024) != 0)
		return NULL;
	if (morph_buf_printf(&buf,
		"tool_create failed\n"
		"stage: check_js\n"
		"rc: -22\n"
		"code: EINVAL\n"
		"detail: JavaScript validation failed for %s.\n\n"
		"Runner output:\n%s\n\n"
		"next_action: Fix the source_js and call tool_create again. The script "
		"must parse, must define global run(args), and run(args) "
		"must return a JSON-serializable value.",
		source_path ? source_path : DYN_TOOL_SOURCE_FILE,
		output && *output ? output : "(runner produced no output)") != 0) {
		morph_buf_cleanup(&buf);
		return NULL;
	}
	return morph_buf_detach(&buf);
}

static int tool_create_fail(struct tool_result *result, const char *stage,
			    int rc, const char *detail)
{
	return tool_result_errorf(result, "tool_failed",
		"tool_create failed\n"
		"stage: %s\n"
		"rc: %d\n"
		"code: %s\n"
		"detail: %s\n"
		"next_action: Fix the field or source for this stage and "
		"call tool_create again.",
		stage ? stage : "unknown",
		rc,
		morph_strerror(rc),
		detail && *detail ? detail : "No additional diagnostic was "
		"provided by this stage.");
}

static int write_tool_files(struct dynamic_tool *dt, const char *source)
{
	cJSON *meta = NULL;
	char *meta_json = NULL;
	char meta_path[PATH_MAX];
	int rc;

	rc = file_ensure_dir(dt->dir);
	if (rc < 0)
		return rc;
	rc = file_write_all(dt->source_path, source, strlen(source));
	if (rc < 0)
		return rc;
	rc = file_path_join(meta_path, sizeof(meta_path), dt->dir,
			    DYN_TOOL_META_FILE);
	if (rc < 0)
		return rc;
	meta = cJSON_CreateObject();
	if (!meta)
		return -ENOMEM;
	cJSON_AddStringToObject(meta, "name", dt->name);
	cJSON_AddStringToObject(meta, "description", dt->description);
	cJSON_AddStringToObject(meta, "input_schema", dt->input_schema);
	cJSON_AddStringToObject(meta, "output_schema", dt->output_schema);
	meta_json = cJSON_PrintUnformatted(meta);
	cJSON_Delete(meta);
	if (!meta_json)
		return -ENOMEM;
	rc = file_write_all(meta_path, meta_json, strlen(meta_json));
	free(meta_json);
	return rc;
}

static int tool_file_paths(const char *dir, char *meta_path, size_t meta_size,
			   char *source_path, size_t source_size)
{
	int rc;

	if (!dir)
		return -EINVAL;
	rc = file_path_join(meta_path, meta_size, dir, DYN_TOOL_META_FILE);
	if (rc < 0)
		return rc;
	return file_path_join(source_path, source_size, dir,
			      DYN_TOOL_SOURCE_FILE);
}

static int history_dir_path(const char *tool_dir, char *path, size_t path_size)
{
	return file_path_join(path, path_size, tool_dir, DYN_TOOL_HISTORY_DIR);
}

static int checkpoint_dir_path(const char *tool_dir, const char *checkpoint_id,
			       char *path, size_t path_size)
{
	char history[PATH_MAX];
	int rc;

	rc = history_dir_path(tool_dir, history, sizeof(history));
	if (rc < 0)
		return rc;
	return file_path_join(path, path_size, history, checkpoint_id);
}

static uint64_t fnv1a64(const char *data, size_t len)
{
	uint64_t hash = 1469598103934665603ULL;

	for (size_t i = 0; i < len; i++) {
		hash ^= (unsigned char)data[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

static void hash_text(const char *data, size_t len, char out[32])
{
	snprintf(out, 32, "%016llx",
		 (unsigned long long)fnv1a64(data ? data : "", len));
}

static const char *origin_name(enum tool_origin origin)
{
	if (origin == TOOL_ORIGIN_DYNAMIC_SESSION)
		return "session";
	if (origin == TOOL_ORIGIN_DYNAMIC_PERSISTENT)
		return "persistent";
	return "unknown";
}

static int checkpoint_id_value(const char *id)
{
	int value = 0;

	if (!id || !*id)
		return -1;
	for (const char *p = id; *p; p++) {
		if (*p < '0' || *p > '9')
			return -1;
		value = value * 10 + (*p - '0');
	}
	return value;
}

static int latest_checkpoint_id(const char *tool_dir, char *id, size_t id_size)
{
	char history[PATH_MAX];
	char **dirs = NULL;
	int count = 0;
	int max_id = -1;
	int rc;

	if (!id || id_size == 0)
		return -EINVAL;
	id[0] = '\0';
	rc = history_dir_path(tool_dir, history, sizeof(history));
	if (rc < 0)
		return rc;
	if (!file_exists(history))
		return -ENOENT;
	if (file_list_dirs(history, &dirs, &count) != 0)
		return -ENOENT;
	for (int i = 0; i < count; i++) {
		int value = checkpoint_id_value(dirs[i]);
		if (value > max_id)
			max_id = value;
	}
	file_free_list(dirs, count);
	if (max_id < 0)
		return -ENOENT;
	if (snprintf(id, id_size, "%04d", max_id) >= (int)id_size)
		return -ENAMETOOLONG;
	return 0;
}

static int next_checkpoint_id(const char *tool_dir, char *id, size_t id_size)
{
	char latest[16];
	int rc;
	int value = 0;

	rc = latest_checkpoint_id(tool_dir, latest, sizeof(latest));
	if (rc == 0)
		value = checkpoint_id_value(latest);
	else if (rc != -ENOENT)
		return rc;
	value++;
	if (snprintf(id, id_size, "%04d", value) >= (int)id_size)
		return -ENAMETOOLONG;
	return 0;
}

static int write_checkpoint_meta(const char *checkpoint_dir,
				 const struct tool_checkpoint *cp)
{
	char path[PATH_MAX];
	cJSON *root;
	char *json;
	int rc;

	rc = file_path_join(path, sizeof(path), checkpoint_dir,
			    DYN_TOOL_CHECKPOINT_META_FILE);
	if (rc < 0)
		return rc;
	root = cJSON_CreateObject();
	if (!root)
		return -ENOMEM;
	cJSON_AddStringToObject(root, "id", cp->id);
	cJSON_AddStringToObject(root, "name", cp->name);
	cJSON_AddStringToObject(root, "before_state", cp->before_state);
	cJSON_AddStringToObject(root, "origin", cp->origin);
	cJSON_AddStringToObject(root, "old_hash", cp->old_hash);
	cJSON_AddStringToObject(root, "new_hash", cp->new_hash);
	cJSON_AddNumberToObject(root, "created_at", (double)cp->created_at);
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json)
		return -ENOMEM;
	rc = file_write_all(path, json, strlen(json));
	free(json);
	return rc;
}

static int read_checkpoint_meta(const char *checkpoint_dir,
				struct tool_checkpoint *cp)
{
	char path[PATH_MAX];
	char *json;
	cJSON *root;
	cJSON *item;
	int rc;

	if (!cp)
		return -EINVAL;
	memset(cp, 0, sizeof(*cp));
	rc = file_path_join(path, sizeof(path), checkpoint_dir,
			    DYN_TOOL_CHECKPOINT_META_FILE);
	if (rc < 0)
		return rc;
	json = file_read_all(path, NULL);
	if (!json)
		return -ENOENT;
	root = cJSON_Parse(json);
	free(json);
	if (!root)
		return -EINVAL;
	item = cJSON_GetObjectItem(root, "id");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(cp->id, item->valuestring, sizeof(cp->id) - 1);
	item = cJSON_GetObjectItem(root, "name");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(cp->name, item->valuestring, sizeof(cp->name) - 1);
	item = cJSON_GetObjectItem(root, "before_state");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(cp->before_state, item->valuestring,
			sizeof(cp->before_state) - 1);
	item = cJSON_GetObjectItem(root, "origin");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(cp->origin, item->valuestring, sizeof(cp->origin) - 1);
	item = cJSON_GetObjectItem(root, "old_hash");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(cp->old_hash, item->valuestring,
			sizeof(cp->old_hash) - 1);
	item = cJSON_GetObjectItem(root, "new_hash");
	if (cJSON_IsString(item) && item->valuestring)
		strncpy(cp->new_hash, item->valuestring,
			sizeof(cp->new_hash) - 1);
	item = cJSON_GetObjectItem(root, "created_at");
	if (cJSON_IsNumber(item))
		cp->created_at = (long)item->valuedouble;
	cJSON_Delete(root);
	if (!cp->id[0] || !cp->name[0] || !cp->before_state[0])
		return -EINVAL;
	return 0;
}

static int remove_checkpoint_ids_from(const char *tool_dir, int min_id)
{
	char history[PATH_MAX];
	char **dirs = NULL;
	int count = 0;
	int rc;

	rc = history_dir_path(tool_dir, history, sizeof(history));
	if (rc < 0)
		return rc;
	if (!file_exists(history))
		return 0;
	if (file_list_dirs(history, &dirs, &count) != 0)
		return 0;
	for (int i = 0; i < count; i++) {
		int value = checkpoint_id_value(dirs[i]);
		char path[PATH_MAX];

		if (value < min_id)
			continue;
		if (file_path_join(path, sizeof(path), history, dirs[i]) != 0)
			continue;
		(void)remove_tree(path);
	}
	file_free_list(dirs, count);
	return 0;
}

static int prune_checkpoint_history(const char *tool_dir)
{
	char history[PATH_MAX];
	char **dirs = NULL;
	int count = 0;
	int rc;

	rc = history_dir_path(tool_dir, history, sizeof(history));
	if (rc < 0)
		return rc;
	if (!file_exists(history))
		return 0;
	if (file_list_dirs(history, &dirs, &count) != 0)
		return 0;
	while (count > DYN_TOOL_HISTORY_LIMIT) {
		int min_idx = -1;
		int min_id = INT_MAX;

		for (int i = 0; i < count; i++) {
			int value = checkpoint_id_value(dirs[i]);
			if (value >= 0 && value < min_id) {
				min_id = value;
				min_idx = i;
			}
		}
		if (min_idx < 0)
			break;
		{
			char path[PATH_MAX];
			if (file_path_join(path, sizeof(path), history,
					   dirs[min_idx]) == 0)
				(void)remove_tree(path);
		}
		free(dirs[min_idx]);
		for (int i = min_idx; i < count - 1; i++)
			dirs[i] = dirs[i + 1];
		count--;
	}
	file_free_list(dirs, count);
	return 0;
}

static int create_tool_checkpoint(const char *tool_dir,
				  const struct tool_entry *existing,
				  const char *name,
				  struct tool_checkpoint *cp,
				  char *checkpoint_dir,
				  size_t checkpoint_dir_size)
{
	char history[PATH_MAX];
	char id[16];
	char old_meta[PATH_MAX];
	char old_source[PATH_MAX];
	char cp_meta[PATH_MAX];
	char cp_source[PATH_MAX];
	struct dynamic_tool *old_dt = NULL;
	char *source = NULL;
	size_t source_len = 0;
	int rc;

	if (!tool_dir || !name || !cp || !checkpoint_dir)
		return -EINVAL;
	memset(cp, 0, sizeof(*cp));
	rc = file_ensure_dir(tool_dir);
	if (rc < 0)
		return rc;
	rc = history_dir_path(tool_dir, history, sizeof(history));
	if (rc == 0)
		rc = file_ensure_dir(history);
	if (rc == 0)
		rc = next_checkpoint_id(tool_dir, id, sizeof(id));
	if (rc == 0)
		rc = checkpoint_dir_path(tool_dir, id, checkpoint_dir,
					 checkpoint_dir_size);
	if (rc == 0)
		rc = file_ensure_dir(checkpoint_dir);
	if (rc < 0)
		return rc;
	strncpy(cp->id, id, sizeof(cp->id) - 1);
	strncpy(cp->name, name, sizeof(cp->name) - 1);
	strncpy(cp->before_state, "absent", sizeof(cp->before_state) - 1);
	strncpy(cp->origin, "none", sizeof(cp->origin) - 1);
	strncpy(cp->old_hash, "absent", sizeof(cp->old_hash) - 1);
	cp->created_at = (long)time(NULL);
	if (existing && (existing->flags & TOOL_FLAG_DYNAMIC) &&
	    existing->user_data) {
		old_dt = existing->user_data;
		strncpy(cp->before_state, "present",
			sizeof(cp->before_state) - 1);
		strncpy(cp->origin, origin_name(existing->origin),
			sizeof(cp->origin) - 1);
		rc = tool_file_paths(old_dt->dir, old_meta, sizeof(old_meta),
				     old_source, sizeof(old_source));
		if (rc == 0)
			rc = tool_file_paths(checkpoint_dir, cp_meta,
					     sizeof(cp_meta), cp_source,
					     sizeof(cp_source));
		if (rc == 0)
			rc = copy_file(old_meta, cp_meta);
		if (rc == 0)
			rc = copy_file(old_source, cp_source);
		if (rc == 0) {
			source = file_read_all(old_source, &source_len);
			if (!source)
				rc = -ENOENT;
		}
		if (rc == 0) {
			hash_text(source, source_len, cp->old_hash);
			free(source);
			source = NULL;
		}
	}
	if (rc == 0)
		rc = write_checkpoint_meta(checkpoint_dir, cp);
	if (source)
		free(source);
	if (rc < 0)
		(void)remove_tree(checkpoint_dir);
	return rc;
}

static int restore_tool_checkpoint(const char *tool_dir,
				   const char *checkpoint_dir,
				   const struct tool_checkpoint *cp)
{
	char old_meta[PATH_MAX];
	char old_source[PATH_MAX];
	char dst_meta[PATH_MAX];
	char dst_source[PATH_MAX];
	int rc;

	if (!tool_dir || !checkpoint_dir || !cp)
		return -EINVAL;
	if (strcmp(cp->before_state, "absent") == 0) {
		if (file_exists(tool_dir))
			return remove_tree(tool_dir);
		return 0;
	}
	if (strcmp(cp->before_state, "present") != 0)
		return -EINVAL;
	rc = file_ensure_dir(tool_dir);
	if (rc == 0)
		rc = tool_file_paths(checkpoint_dir, old_meta, sizeof(old_meta),
				     old_source, sizeof(old_source));
	if (rc == 0)
		rc = tool_file_paths(tool_dir, dst_meta, sizeof(dst_meta),
				     dst_source, sizeof(dst_source));
	if (rc == 0)
		rc = copy_file(old_meta, dst_meta);
	if (rc == 0)
		rc = copy_file(old_source, dst_source);
	return rc;
}

static int check_js_source(const char *source_path,
			   char **error_out)
{
	pid_t pid;
	int output_pipe[2] = {-1, -1};
	int status = 0;

	if (error_out)
		*error_out = NULL;
	if (pipe(output_pipe) < 0)
		MORPH_RETURN_ERRNO();
	pid = fork();
	if (pid < 0) {
		close(output_pipe[0]);
		close(output_pipe[1]);
		MORPH_RETURN_ERRNO();
	}
	if (pid == 0) {
		int first_errno;
		close(output_pipe[0]);
		dup2(output_pipe[1], STDOUT_FILENO);
		dup2(output_pipe[1], STDERR_FILENO);
		close(output_pipe[1]);
		const char *runner = runner_path();
		execl(runner, runner, "--check",
		      source_path, (char *)NULL);
		first_errno = errno;
		execlp("morph-js-runner", "morph-js-runner", "--check",
		       source_path, (char *)NULL);
		fprintf(stderr,
			"failed to exec morph-js-runner. runner=%s "
			"source=%s direct_errno=%s fallback_errno=%s\n",
			runner ? runner : "(null)",
			source_path ? source_path : "(null)",
			strerror(first_errno), strerror(errno));
		_exit(127);
	}
	close(output_pipe[1]);
	if (error_out) {
		morph_buf_t err;
		char tmp[512];
		if (morph_buf_init(&err, 512) == 0) {
			for (;;) {
				ssize_t n = read(output_pipe[0], tmp, sizeof(tmp));
				if (n < 0 && errno == EINTR)
					continue;
				if (n <= 0)
					break;
				if (err.len < 8192) {
					size_t keep = (size_t)n;
					if (err.len + keep > 8192)
						keep = 8192 - err.len;
					(void)morph_buf_append(&err, tmp, keep);
				}
			}
			if (err.len > 0)
				*error_out = morph_buf_detach(&err);
			else
				morph_buf_cleanup(&err);
		}
	}
	close(output_pipe[0]);
	if (waitpid(pid, &status, 0) < 0)
		MORPH_RETURN_ERRNO();
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		if (error_out && !*error_out) {
			morph_buf_t err;
			if (morph_buf_init(&err, 256) == 0) {
				if (WIFEXITED(status)) {
					(void)morph_buf_printf(&err,
						"morph-js-runner --check failed "
						"without output. runner=%s "
						"source=%s exit_status=%d",
						runner_path(),
						source_path ? source_path :
						"(null)",
						WEXITSTATUS(status));
				} else if (WIFSIGNALED(status)) {
					(void)morph_buf_printf(&err,
						"morph-js-runner --check was "
						"terminated by signal %d. "
						"runner=%s source=%s",
						WTERMSIG(status), runner_path(),
						source_path ? source_path :
						"(null)");
				} else {
					(void)morph_buf_printf(&err,
						"morph-js-runner --check failed "
						"without output. runner=%s "
						"source=%s status=%d",
						runner_path(),
						source_path ? source_path :
						"(null)", status);
				}
				*error_out = morph_buf_detach(&err);
			}
		}
		return -EINVAL;
	}
	return 0;
}

static int load_tool_from_dir(struct tool_registry *reg,
			      struct tool_context *tctx,
			      const struct config_dynamic_tools *cfg,
			      const char *dir,
			      enum tool_origin origin)
{
	char meta_path[PATH_MAX];
	char source_path[PATH_MAX];
	char *meta_text;
	cJSON *meta;
	struct dynamic_tool *dt;
	char *schema;
	int rc;

	if (!reg || !cfg || !dir)
		return -EINVAL;
	rc = file_path_join(meta_path, sizeof(meta_path), dir,
			    DYN_TOOL_META_FILE);
	if (rc < 0)
		return rc;
	rc = file_path_join(source_path, sizeof(source_path), dir,
			    DYN_TOOL_SOURCE_FILE);
	if (rc < 0)
		return rc;
	if (!file_exists(meta_path) || !file_exists(source_path))
		return -ENOENT;
	meta_text = file_read_all(meta_path, NULL);
	if (!meta_text)
		return -ENOENT;
	meta = cJSON_Parse(meta_text);
	free(meta_text);
	if (!meta)
		return -EINVAL;
	dt = calloc(1, sizeof(*dt));
	if (!dt) {
		cJSON_Delete(meta);
		return -ENOMEM;
	}
	{
			cJSON *name = cJSON_GetObjectItem(meta, "name");
			cJSON *desc = cJSON_GetObjectItem(meta, "description");
			cJSON *args = cJSON_GetObjectItem(meta, "input_schema");
			cJSON *output = cJSON_GetObjectItem(meta, "output_schema");
		if (!cJSON_IsString(name) || !name->valuestring ||
		    !valid_tool_name(name->valuestring)) {
			cJSON_Delete(meta);
			free(dt);
			return -EINVAL;
		}
		strncpy(dt->name, name->valuestring, sizeof(dt->name) - 1);
			if (cJSON_IsString(desc) && desc->valuestring)
				strncpy(dt->description, desc->valuestring,
					sizeof(dt->description) - 1);
			if (!args || (cJSON_IsObject(args) &&
			    !cJSON_GetObjectItem(args, "properties"))) {
				schema = strdup(TOOL_EMPTY_INPUT_SCHEMA);
			} else {
				schema = schema_to_string(args);
			}
			if (!schema) {
				cJSON_Delete(meta);
				free(dt);
				return -ENOMEM;
			}
			strncpy(dt->input_schema, schema,
				sizeof(dt->input_schema) - 1);
			free(schema);
			if (!output) {
				schema = strdup(TOOL_OBJECT_OUTPUT_SCHEMA);
			} else {
				schema = schema_to_string(output);
			}
			if (!schema) {
				cJSON_Delete(meta);
				free(dt);
				return -ENOMEM;
			}
			strncpy(dt->output_schema, schema,
				sizeof(dt->output_schema) - 1);
			free(schema);
		}
	strncpy(dt->source_path, source_path, sizeof(dt->source_path) - 1);
	strncpy(dt->dir, dir, sizeof(dt->dir) - 1);
	dt->cfg = cfg;
	dt->tctx = tctx;
	dt->origin = origin;
	cJSON_Delete(meta);
	if (tool_lookup(reg, dt->name) &&
	    !(tool_lookup(reg, dt->name)->flags & TOOL_FLAG_DYNAMIC)) {
		free(dt);
		return -EEXIST;
	}
	return register_dynamic_tool(reg, dt);
}

static int create_context_dir(const struct dynamic_tools_context *ctx,
			      const char *name, char *dir, size_t dir_size)
{
	char session_root[PATH_MAX];
	int rc;

	if (!ctx || !ctx->session_id[0])
		return -EINVAL;
	rc = file_path_join(session_root, sizeof(session_root),
			    ctx->cfg->session_dir, ctx->session_id);
	if (rc < 0)
		return rc;
	return file_path_join(dir, dir_size, session_root, name);
}

static int tool_create_exec(const char *args_json, struct tool_result *result,
			    void *user_data)
{
	struct dynamic_tools_context *ctx = user_data;
	cJSON *root;
	cJSON *name_item;
	cJSON *desc_item;
	cJSON *source_item;
	char *schema = NULL;
	char *output_schema = NULL;
	char *check_error = NULL;
	struct dynamic_tool *dt = NULL;
	struct tool_entry *existing = NULL;
	struct tool_checkpoint checkpoint;
	char checkpoint_dir[PATH_MAX];
	char new_hash[32];
	int checkpoint_created = 0;
	int register_rc = 0;
	const char *stage = "init";
	int rc;

	if (!ctx || !result)
		return -EINVAL;
	if (!ctx->cfg || !ctx->cfg->enabled)
		return tool_result_error(result, "tool_failed", "dynamic tools disabled");
	stage = "parse_args";
	root = args_json ? cJSON_Parse(args_json) : NULL;
	if (!root)
		return tool_create_fail(result, stage, -EINVAL,
					"tool_create arguments are not valid "
					"JSON.");
	name_item = cJSON_GetObjectItem(root, "name");
	desc_item = cJSON_GetObjectItem(root, "description");
	source_item = cJSON_GetObjectItem(root, "source_js");
	{
		morph_buf_t arg_err;
		if (morph_buf_init(&arg_err, 512) != 0) {
			cJSON_Delete(root);
			return tool_create_fail(result, stage, -ENOMEM,
						"failed to allocate argument "
						"diagnostic buffer");
		}
		rc = validate_tool_create_args(root, name_item, source_item,
					       &arg_err);
		if (rc != 0 || arg_err.len > 0) {
			morph_buf_t detail;
			int out_rc;
			if (morph_buf_init(&detail, 768) != 0) {
				morph_buf_cleanup(&arg_err);
				cJSON_Delete(root);
				return tool_create_fail(result, stage, -ENOMEM,
					"failed to allocate argument detail "
					"buffer");
			}
				(void)morph_buf_printf(&detail,
					"%s\n"
					"Expected shape: {\"name\":\"my_tool\","
					"\"description\":\"...\","
					"\"input_schema\":{\"type\":\"object\","
					"\"properties\":{}},"
					"\"output_schema\":{\"type\":\"object\","
					"\"properties\":{}},"
					"\"source_js\":\"async function run(args) { "
					"return { value: true }; }\"}",
				morph_buf_cstr(&arg_err));
			out_rc = tool_create_fail(result, stage, -EINVAL,
						  morph_buf_cstr(&detail));
			morph_buf_cleanup(&detail);
			morph_buf_cleanup(&arg_err);
			cJSON_Delete(root);
			return out_rc;
		}
		morph_buf_cleanup(&arg_err);
	}
	stage = "validate_source_size";
	if ((int)strlen(source_item->valuestring) >
	    ctx->cfg->max_source_bytes) {
		int len = (int)strlen(source_item->valuestring);
		int max_len = ctx->cfg->max_source_bytes;
		morph_buf_t detail;
		int out_rc;
		if (morph_buf_init(&detail, 128) != 0) {
			cJSON_Delete(root);
			return tool_create_fail(result, stage, -ENOMEM,
						"failed to allocate source size "
						"diagnostic buffer");
		}
		(void)morph_buf_printf(&detail,
			"source_js too large: got %d bytes, maximum is %d "
			"bytes. Split the logic or remove embedded data.",
			len, max_len);
		out_rc = tool_create_fail(result, stage, -EFBIG,
					  morph_buf_cstr(&detail));
		morph_buf_cleanup(&detail);
		cJSON_Delete(root);
		return out_rc;
	}
	stage = "check_name_conflict";
	{
		existing = tool_lookup(ctx->reg, name_item->valuestring);
		if (existing && !(existing->flags & TOOL_FLAG_DYNAMIC)) {
			cJSON_Delete(root);
			return tool_create_fail(result, stage, -EEXIST,
						"tool name already exists and is "
						"not a dynamic tool");
		}
	}
	hash_text(source_item->valuestring, strlen(source_item->valuestring),
		  new_hash);
	stage = "approval";
	if (ctx->cfg->create_requires_approval && ctx->tctx) {
		struct tool_operation op;
		memset(&op, 0, sizeof(op));
		op.kind = TOOL_OP_EXTERNAL_SEND;
		op.tool_name = "tool_create";
		op.action = "create dynamic tool";
		op.target = name_item->valuestring;
		op.details_json = args_json;
		rc = tool_context_check_operation(ctx->tctx, &op);
		if (rc < 0) {
			cJSON_Delete(root);
			return tool_create_fail(result, stage, rc,
						"dynamic tool creation denied");
		}
	}
	stage = "validate_input_schema";
	schema = schema_to_string(cJSON_GetObjectItem(root, "input_schema"));
	if (!schema) {
		cJSON_Delete(root);
		return tool_create_fail(result, stage, -ENOMEM,
					"failed to serialize input_schema");
	}
		{
			cJSON *schema_root = cJSON_Parse(schema);
		if (!schema_root) {
			const char *pos = cJSON_GetErrorPtr();
			morph_buf_t detail;
			int out_rc;
			if (morph_buf_init(&detail, 256) != 0) {
				free(schema);
				cJSON_Delete(root);
				return tool_create_fail(result, stage, -ENOMEM,
					"failed to allocate input_schema "
					"diagnostic buffer");
			}
			(void)morph_buf_printf(&detail,
				"invalid input_schema JSON near: %.80s\n"
				"input_schema must be a JSON Schema object or a "
				"string containing valid JSON Schema.",
				pos ? pos : "(unknown)");
			out_rc = tool_create_fail(result, stage, -EINVAL,
						  morph_buf_cstr(&detail));
			morph_buf_cleanup(&detail);
			free(schema);
			cJSON_Delete(root);
			return out_rc;
		}
			cJSON_Delete(schema_root);
		}
	stage = "validate_output_schema";
	output_schema = schema_to_string(cJSON_GetObjectItem(root,
							     "output_schema"));
	if (!output_schema) {
		free(schema);
		cJSON_Delete(root);
		return tool_create_fail(result, stage, -ENOMEM,
					"failed to serialize output_schema");
	}
	{
		cJSON *schema_root = cJSON_Parse(output_schema);
		if (!schema_root) {
			free(schema);
			free(output_schema);
			cJSON_Delete(root);
			return tool_create_fail(result, stage, -EINVAL,
						"invalid output_schema JSON");
		}
		cJSON_Delete(schema_root);
	}
	stage = "allocate_tool";
	dt = calloc(1, sizeof(*dt));
	if (!dt) {
		free(schema);
		free(output_schema);
		cJSON_Delete(root);
		return tool_create_fail(result, stage, -ENOMEM,
					"failed to allocate dynamic tool");
	}
	strncpy(dt->name, name_item->valuestring, sizeof(dt->name) - 1);
	if (cJSON_IsString(desc_item) && desc_item->valuestring)
		strncpy(dt->description, desc_item->valuestring,
			sizeof(dt->description) - 1);
	else
		snprintf(dt->description, sizeof(dt->description),
			 "Dynamic JS tool %s", dt->name);
	strncpy(dt->input_schema, schema, sizeof(dt->input_schema) - 1);
	free(schema);
	strncpy(dt->output_schema, output_schema, sizeof(dt->output_schema) - 1);
	free(output_schema);
	dt->cfg = ctx->cfg;
	dt->tctx = ctx->tctx;
	dt->origin = TOOL_ORIGIN_DYNAMIC_SESSION;
	stage = "create_tool_dir";
	rc = create_context_dir(ctx, dt->name, dt->dir, sizeof(dt->dir));
	if (rc == 0) {
		stage = "build_source_path";
		rc = file_path_join(dt->source_path, sizeof(dt->source_path),
				    dt->dir, DYN_TOOL_SOURCE_FILE);
	}
	if (rc == 0) {
		stage = "create_checkpoint";
		rc = create_tool_checkpoint(dt->dir, existing, dt->name,
					    &checkpoint, checkpoint_dir,
					    sizeof(checkpoint_dir));
		if (rc == 0) {
			strncpy(checkpoint.new_hash, new_hash,
				sizeof(checkpoint.new_hash) - 1);
			checkpoint_created = 1;
		}
	}
	if (rc == 0) {
		stage = "write_tool_files";
		rc = write_tool_files(dt, source_item->valuestring);
	}
	if (rc == 0) {
		stage = "check_js";
		rc = check_js_source(dt->source_path, &check_error);
	}
	if (rc == 0 && check_error) {
		free(check_error);
		check_error = NULL;
	}
	if (rc == 0) {
		stage = "register_tool";
		rc = register_dynamic_tool(ctx->reg, dt);
		register_rc = rc;
	}
	if (rc == 0 && checkpoint_created) {
		int meta_rc;

		stage = "update_checkpoint";
		meta_rc = write_checkpoint_meta(checkpoint_dir, &checkpoint);
		if (meta_rc == 0)
			meta_rc = prune_checkpoint_history(dt->dir);
		if (meta_rc < 0)
			log_warn("failed to update dynamic tool checkpoint: %s",
				 morph_strerror(meta_rc));
	}
	cJSON_Delete(root);
	if (rc < 0) {
		if (checkpoint_created) {
			(void)restore_tool_checkpoint(dt ? dt->dir : NULL,
						      checkpoint_dir,
						      &checkpoint);
			(void)remove_tree(checkpoint_dir);
		}
		if (check_error) {
			char *formatted = format_check_error(dt ? dt->source_path :
							     NULL, check_error);
			int out_rc;
			free(dt);
			if (formatted) {
				out_rc = tool_result_error(result, "tool_failed",
								formatted);
				free(formatted);
			} else {
				out_rc = tool_create_fail(result, stage, rc,
							  check_error);
			}
			free(check_error);
			return out_rc;
		}
		free(dt);
		return tool_create_fail(result, stage, rc, NULL);
	}
	return tool_result_successf(result,
				  "{\"name\":\"%s\",\"status\":\"%s\","
				  "\"checkpoint_id\":\"%s\","
				  "\"diff_available\":true,"
				  "\"old_hash\":\"%s\","
				  "\"new_hash\":\"%s\"}",
				  dt->name,
				  register_rc > 0 ? "updated" : "registered",
				  checkpoint.id,
				  checkpoint.old_hash,
				  new_hash);
}

static int copy_file(const char *src, const char *dst)
{
	char *data;
	size_t len;
	int rc;

	data = file_read_all(src, &len);
	if (!data)
		return -ENOENT;
	rc = file_write_all(dst, data, len);
	free(data);
	return rc;
}

static int remove_tree(const char *path)
{
	struct stat st;
	DIR *dir;
	struct dirent *ent;

	if (!path || !*path)
		return -EINVAL;
	if (lstat(path, &st) != 0)
		return -errno;
	if (!S_ISDIR(st.st_mode)) {
		if (unlink(path) != 0)
			return -errno;
		return 0;
	}
	dir = opendir(path);
	if (!dir)
		return -errno;
	while ((ent = readdir(dir)) != NULL) {
		char child[PATH_MAX];
		int rc;

		if (strcmp(ent->d_name, ".") == 0 ||
		    strcmp(ent->d_name, "..") == 0)
			continue;
		rc = file_path_join(child, sizeof(child), path, ent->d_name);
		if (rc < 0) {
			closedir(dir);
			return rc;
		}
		rc = remove_tree(child);
		if (rc < 0) {
			closedir(dir);
			return rc;
		}
	}
	closedir(dir);
	if (rmdir(path) != 0)
		return -errno;
	return 0;
}

static int dynamic_tool_delete_path_allowed(
	const struct dynamic_tools_context *ctx,
	enum tool_origin origin,
	const char *name,
	const char *dir)
{
	char expected[PATH_MAX];
	char session_root[PATH_MAX];
	char *resolved_dir = NULL;
	char *resolved_expected = NULL;
	int rc;
	int allowed;

	if (!ctx || !ctx->cfg || !name || !dir)
		return 0;
	if (origin == TOOL_ORIGIN_DYNAMIC_SESSION) {
		if (!ctx->session_id[0])
			return 0;
		rc = file_path_join(session_root, sizeof(session_root),
				    ctx->cfg->session_dir, ctx->session_id);
		if (rc < 0)
			return 0;
		rc = file_path_join(expected, sizeof(expected),
				    session_root, name);
	} else if (origin == TOOL_ORIGIN_DYNAMIC_PERSISTENT) {
		rc = file_path_join(expected, sizeof(expected),
				    ctx->cfg->persistent_dir, name);
	} else {
		return 0;
	}
	if (rc < 0)
		return 0;
	resolved_dir = file_resolve_path(dir);
	resolved_expected = file_resolve_path(expected);
	allowed = resolved_dir && resolved_expected &&
		strcmp(resolved_dir, resolved_expected) == 0;
	free(resolved_dir);
	free(resolved_expected);
	return allowed;
}

static int tool_promote_exec(const char *args_json, struct tool_result *result,
			     void *user_data)
{
	struct dynamic_tools_context *ctx = user_data;
	cJSON *root;
	cJSON *name_item;
	char src_dir[PATH_MAX];
	char dst_dir[PATH_MAX];
	char src_meta[PATH_MAX];
	char src_js[PATH_MAX];
	char dst_meta[PATH_MAX];
	char dst_js[PATH_MAX];
	char name[TOOL_NAME_MAX];
	int rc;

	if (!ctx || !result)
		return -EINVAL;
	root = args_json ? cJSON_Parse(args_json) : NULL;
	if (!root)
		return tool_result_error(result, "tool_failed", "invalid JSON");
	name_item = cJSON_GetObjectItem(root, "name");
	if (!cJSON_IsString(name_item) || !valid_tool_name(name_item->valuestring)) {
		cJSON_Delete(root);
		return tool_result_error(result, "tool_failed",
					      "tool_promote requires valid name");
	}
	strncpy(name, name_item->valuestring, sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	if (ctx->cfg->promote_requires_approval && ctx->tctx) {
		struct tool_operation op;
		memset(&op, 0, sizeof(op));
		op.kind = TOOL_OP_EXTERNAL_SEND;
		op.tool_name = "tool_promote";
		op.action = "promote dynamic tool";
		op.target = name;
		op.details_json = args_json;
		rc = tool_context_check_operation(ctx->tctx, &op);
		if (rc < 0) {
			cJSON_Delete(root);
			return tool_result_error(result, "tool_failed",
				"dynamic tool promotion denied");
		}
	}
	rc = create_context_dir(ctx, name, src_dir, sizeof(src_dir));
	if (rc == 0)
		rc = file_path_join(dst_dir, sizeof(dst_dir),
				    ctx->cfg->persistent_dir, name);
	if (rc == 0 && file_exists(dst_dir))
		rc = -EEXIST;
	if (rc == 0)
		rc = file_ensure_dir(dst_dir);
	if (rc == 0)
		rc = file_path_join(src_meta, sizeof(src_meta), src_dir,
				    DYN_TOOL_META_FILE);
	if (rc == 0)
		rc = file_path_join(src_js, sizeof(src_js), src_dir,
				    DYN_TOOL_SOURCE_FILE);
	if (rc == 0)
		rc = file_path_join(dst_meta, sizeof(dst_meta), dst_dir,
				    DYN_TOOL_META_FILE);
	if (rc == 0)
		rc = file_path_join(dst_js, sizeof(dst_js), dst_dir,
				    DYN_TOOL_SOURCE_FILE);
	if (rc == 0)
		rc = copy_file(src_meta, dst_meta);
	if (rc == 0)
		rc = copy_file(src_js, dst_js);
	if (rc == 0)
		rc = load_tool_from_dir(ctx->reg, ctx->tctx, ctx->cfg, dst_dir,
					TOOL_ORIGIN_DYNAMIC_PERSISTENT);
	cJSON_Delete(root);
	if (rc < 0)
		return tool_result_errorf(result, "tool_failed",
			"failed to promote dynamic tool: %s", morph_strerror(rc));
	return tool_result_successf(result,
				  "{\"name\":\"%s\",\"status\":\"promoted\"}",
				  name);
}

static int tool_delete_exec(const char *args_json, struct tool_result *result,
			    void *user_data)
{
	struct dynamic_tools_context *ctx = user_data;
	struct tool_entry *entry;
	struct dynamic_tool *dt;
	enum tool_origin origin;
	cJSON *root;
	cJSON *name_item;
	char name[TOOL_NAME_MAX];
	char dir[PATH_MAX];
	int rc;

	if (!ctx || !result)
		return -EINVAL;
	root = args_json ? cJSON_Parse(args_json) : NULL;
	if (!root)
		return tool_result_error(result, "tool_failed", "invalid JSON");
	name_item = cJSON_GetObjectItem(root, "name");
	if (!cJSON_IsString(name_item) ||
	    !valid_tool_name(name_item->valuestring)) {
		cJSON_Delete(root);
		return tool_result_error(result, "tool_failed",
					      "tool_delete requires valid name");
	}
	strncpy(name, name_item->valuestring, sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	entry = tool_lookup(ctx->reg, name);
	if (!entry) {
		cJSON_Delete(root);
		return tool_result_error(result, "tool_failed", "dynamic tool not found");
	}
	if (!(entry->flags & TOOL_FLAG_DYNAMIC) ||
	    (entry->origin != TOOL_ORIGIN_DYNAMIC_SESSION &&
	     entry->origin != TOOL_ORIGIN_DYNAMIC_PERSISTENT)) {
		cJSON_Delete(root);
		return tool_result_error(result, "tool_failed",
					      "tool_delete can only delete dynamic tools");
	}
	dt = entry->user_data;
	if (!dt || !dt->dir[0]) {
		cJSON_Delete(root);
		return tool_result_error(result, "tool_failed",
					      "dynamic tool metadata missing");
	}
	origin = entry->origin;
	strncpy(dir, dt->dir, sizeof(dir) - 1);
	dir[sizeof(dir) - 1] = '\0';
	if (ctx->cfg->promote_requires_approval && ctx->tctx) {
		struct tool_operation op;
		memset(&op, 0, sizeof(op));
		op.kind = TOOL_OP_EXTERNAL_SEND;
		op.tool_name = "tool_delete";
		op.action = "delete dynamic tool";
		op.target = name;
		op.details_json = args_json;
		rc = tool_context_check_operation(ctx->tctx, &op);
		if (rc < 0) {
			cJSON_Delete(root);
			return tool_result_error(result, "tool_failed",
				"dynamic tool deletion denied");
		}
	}
	if (!dynamic_tool_delete_path_allowed(ctx, origin, name, dir)) {
		cJSON_Delete(root);
		return tool_result_error(result, "tool_failed",
			"dynamic tool deletion refused: path is outside the expected dynamic tools directory");
	}
	rc = remove_tree(dir);
	if (rc == -ENOENT)
		rc = 0;
	if (rc == 0)
		rc = tool_unregister(ctx->reg, name);
	if (rc == 0 && origin == TOOL_ORIGIN_DYNAMIC_SESSION) {
		char persistent_dir[PATH_MAX];
		int load_rc;

		load_rc = file_path_join(persistent_dir,
					 sizeof(persistent_dir),
					 ctx->cfg->persistent_dir, name);
		if (load_rc == 0 && file_exists(persistent_dir)) {
			load_rc = load_tool_from_dir(
				ctx->reg, ctx->tctx, ctx->cfg, persistent_dir,
				TOOL_ORIGIN_DYNAMIC_PERSISTENT);
			if (load_rc < 0)
				rc = load_rc;
		} else if (load_rc < 0) {
			rc = load_rc;
		}
	}
	cJSON_Delete(root);
	if (rc < 0)
		return tool_result_errorf(result, "tool_failed",
			"failed to delete dynamic tool: %s", morph_strerror(rc));
	return tool_result_successf(result,
				  "{\"name\":\"%s\",\"status\":\"deleted\"}",
				  name);
}

static int parse_tool_name_arg(const char *args_json, const char *tool_name,
			       cJSON **root_out, char name[TOOL_NAME_MAX])
{
	cJSON *root;
	cJSON *name_item;

	if (!root_out || !name)
		return -EINVAL;
	*root_out = NULL;
	root = args_json ? cJSON_Parse(args_json) : NULL;
	if (!root)
		return -EINVAL;
	name_item = cJSON_GetObjectItem(root, "name");
	if (!cJSON_IsString(name_item) ||
	    !valid_tool_name(name_item->valuestring)) {
		cJSON_Delete(root);
		return -EINVAL;
	}
	strncpy(name, name_item->valuestring, TOOL_NAME_MAX - 1);
	name[TOOL_NAME_MAX - 1] = '\0';
	*root_out = root;
	(void)tool_name;
	return 0;
}

static int result_take_cjson(struct tool_result *result, cJSON *root)
{
	char *json;

	if (!root)
		return -ENOMEM;
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json)
		return -ENOMEM;
	return tool_result_success_json_text(result, json);
}

static int tool_history_exec(const char *args_json, struct tool_result *result,
			     void *user_data)
{
	struct dynamic_tools_context *ctx = user_data;
	cJSON *args = NULL;
	cJSON *limit_item;
	cJSON *out;
	cJSON *items;
	char name[TOOL_NAME_MAX];
	char tool_dir[PATH_MAX];
	char history[PATH_MAX];
	char **dirs = NULL;
	int count = 0;
	int limit = 20;
	int rc;

	if (!ctx || !result)
		return -EINVAL;
	rc = parse_tool_name_arg(args_json, "tool_history", &args, name);
	if (rc < 0)
		return tool_result_error(result, "tool_failed",
					      "tool_history requires valid name");
	limit_item = cJSON_GetObjectItem(args, "limit");
	if (cJSON_IsNumber(limit_item) && limit_item->valueint > 0)
		limit = limit_item->valueint;
	cJSON_Delete(args);
	rc = create_context_dir(ctx, name, tool_dir, sizeof(tool_dir));
	if (rc == 0)
		rc = history_dir_path(tool_dir, history, sizeof(history));
	if (rc < 0)
		return tool_result_errorf(result, "tool_failed",
			"failed to read tool history: %s", morph_strerror(rc));
	out = cJSON_CreateObject();
	items = cJSON_CreateArray();
	if (!out || !items) {
		cJSON_Delete(out);
		cJSON_Delete(items);
		return -ENOMEM;
	}
	cJSON_AddStringToObject(out, "name", name);
	cJSON_AddItemToObject(out, "checkpoints", items);
	if (file_exists(history) && file_list_dirs(history, &dirs, &count) == 0) {
		int emitted = 0;

		while (emitted < limit) {
			int max_idx = -1;
			int max_id = -1;

			for (int i = 0; i < count; i++) {
				int value = checkpoint_id_value(dirs[i]);
				if (value > max_id) {
					max_id = value;
					max_idx = i;
				}
			}
			if (max_idx < 0)
				break;
			{
				char cp_dir[PATH_MAX];
				struct tool_checkpoint cp;
				cJSON *item;

				if (file_path_join(cp_dir, sizeof(cp_dir),
						   history, dirs[max_idx]) == 0 &&
				    read_checkpoint_meta(cp_dir, &cp) == 0) {
					item = cJSON_CreateObject();
					if (!item) {
						file_free_list(dirs, count);
						cJSON_Delete(out);
						return -ENOMEM;
					}
					cJSON_AddStringToObject(item, "id", cp.id);
					cJSON_AddStringToObject(item,
						"before_state", cp.before_state);
					cJSON_AddStringToObject(item, "origin",
								cp.origin);
					cJSON_AddStringToObject(item, "old_hash",
								cp.old_hash);
					cJSON_AddStringToObject(item, "new_hash",
								cp.new_hash);
					cJSON_AddNumberToObject(item,
						"created_at", (double)cp.created_at);
					cJSON_AddItemToArray(items, item);
					emitted++;
				}
			}
			dirs[max_idx][0] = '\0';
		}
		file_free_list(dirs, count);
	}
	return result_take_cjson(result, out);
}

static char *read_optional_file(const char *path)
{
	if (!path || !file_exists(path))
		return strdup("");
	return file_read_all(path, NULL);
}

static int append_whole_file_diff(morph_buf_t *buf, const char *label,
				  const char *old_text, const char *new_text)
{
	const char *old_ptr = old_text ? old_text : "";
	const char *new_ptr = new_text ? new_text : "";
	int old_lines = 0;
	int new_lines = 0;
	int rc;

	if (strcmp(old_ptr, new_ptr) == 0)
		return 0;
	for (const char *p = old_ptr; *p; p++)
		if (*p == '\n')
			old_lines++;
	if (*old_ptr)
		old_lines++;
	for (const char *p = new_ptr; *p; p++)
		if (*p == '\n')
			new_lines++;
	if (*new_ptr)
		new_lines++;
	rc = morph_buf_printf(buf, "--- a/%s\n+++ b/%s\n@@ -1,%d +1,%d @@\n",
			      label, label, old_lines, new_lines);
	if (rc != 0)
		return rc;
	for (const char *p = old_ptr; *p;) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) + 1 : strlen(p);

		rc = morph_buf_putc(buf, '-');
		if (rc == 0)
			rc = morph_buf_append(buf, p, len);
		if (rc != 0)
			return rc;
		if (!eol)
			rc = morph_buf_putc(buf, '\n');
		if (rc != 0)
			return rc;
		p += len;
	}
	for (const char *p = new_ptr; *p;) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) + 1 : strlen(p);

		rc = morph_buf_putc(buf, '+');
		if (rc == 0)
			rc = morph_buf_append(buf, p, len);
		if (rc != 0)
			return rc;
		if (!eol)
			rc = morph_buf_putc(buf, '\n');
		if (rc != 0)
			return rc;
		p += len;
	}
	return 0;
}

static int tool_diff_exec(const char *args_json, struct tool_result *result,
			  void *user_data)
{
	struct dynamic_tools_context *ctx = user_data;
	struct tool_entry *entry;
	struct dynamic_tool *dt;
	struct tool_checkpoint cp;
	cJSON *args = NULL;
	cJSON *id_item;
	cJSON *out;
	char name[TOOL_NAME_MAX];
	char cp_id[16];
	char cp_dir[PATH_MAX];
	char old_meta_path[PATH_MAX];
	char old_source_path[PATH_MAX];
	char new_meta_path[PATH_MAX];
	char new_source_path[PATH_MAX];
	char *old_meta = NULL;
	char *old_source = NULL;
	char *new_meta = NULL;
	char *new_source = NULL;
	morph_buf_t diff;
	int rc;

	if (!ctx || !result)
		return -EINVAL;
	rc = parse_tool_name_arg(args_json, "tool_diff", &args, name);
	if (rc < 0)
		return tool_result_error(result, "tool_failed",
					      "tool_diff requires valid name");
	id_item = cJSON_GetObjectItem(args, "checkpoint_id");
	if (cJSON_IsString(id_item) && id_item->valuestring &&
	    strcmp(id_item->valuestring, "latest") != 0) {
		strncpy(cp_id, id_item->valuestring, sizeof(cp_id) - 1);
		cp_id[sizeof(cp_id) - 1] = '\0';
	} else {
		char tool_dir[PATH_MAX];
		rc = create_context_dir(ctx, name, tool_dir, sizeof(tool_dir));
		if (rc == 0)
			rc = latest_checkpoint_id(tool_dir, cp_id, sizeof(cp_id));
		if (rc < 0) {
			cJSON_Delete(args);
			return tool_result_error(result, "tool_failed",
						      "checkpoint not found");
		}
	}
	cJSON_Delete(args);
	entry = tool_lookup(ctx->reg, name);
	if (!entry || !(entry->flags & TOOL_FLAG_DYNAMIC) || !entry->user_data)
		return tool_result_error(result, "tool_failed", "dynamic tool not found");
	dt = entry->user_data;
	rc = checkpoint_dir_path(dt->dir, cp_id, cp_dir, sizeof(cp_dir));
	if (rc == 0)
		rc = read_checkpoint_meta(cp_dir, &cp);
	if (rc == 0)
		rc = tool_file_paths(cp_dir, old_meta_path, sizeof(old_meta_path),
				     old_source_path, sizeof(old_source_path));
	if (rc == 0)
		rc = tool_file_paths(dt->dir, new_meta_path, sizeof(new_meta_path),
				     new_source_path, sizeof(new_source_path));
	if (rc < 0)
		return tool_result_error(result, "tool_failed", "checkpoint not found");
	old_meta = read_optional_file(old_meta_path);
	old_source = read_optional_file(old_source_path);
	new_meta = read_optional_file(new_meta_path);
	new_source = read_optional_file(new_source_path);
	if (!old_meta || !old_source || !new_meta || !new_source) {
		free(old_meta);
		free(old_source);
		free(new_meta);
		free(new_source);
		return -ENOMEM;
	}
	rc = morph_buf_init(&diff, 4096);
	if (rc == 0)
		rc = append_whole_file_diff(&diff, DYN_TOOL_META_FILE, old_meta,
					    new_meta);
	if (rc == 0)
		rc = append_whole_file_diff(&diff, DYN_TOOL_SOURCE_FILE,
					    old_source, new_source);
	free(old_meta);
	free(old_source);
	free(new_meta);
	free(new_source);
	if (rc != 0) {
		morph_buf_cleanup(&diff);
		return rc;
	}
	out = cJSON_CreateObject();
	if (!out) {
		morph_buf_cleanup(&diff);
		return -ENOMEM;
	}
	cJSON_AddStringToObject(out, "name", name);
	cJSON_AddStringToObject(out, "checkpoint_id", cp.id);
	cJSON_AddStringToObject(out, "before_state", cp.before_state);
	cJSON_AddStringToObject(out, "diff", morph_buf_cstr(&diff));
	morph_buf_cleanup(&diff);
	return result_take_cjson(result, out);
}

static int reload_persistent_if_present(struct dynamic_tools_context *ctx,
					const char *name)
{
	char persistent_dir[PATH_MAX];
	int rc;

	rc = file_path_join(persistent_dir, sizeof(persistent_dir),
			    ctx->cfg->persistent_dir, name);
	if (rc < 0)
		return rc;
	if (!file_exists(persistent_dir))
		return 0;
	rc = load_tool_from_dir(ctx->reg, ctx->tctx, ctx->cfg, persistent_dir,
				TOOL_ORIGIN_DYNAMIC_PERSISTENT);
	return rc < 0 ? rc : 0;
}

static int tool_rollback_exec(const char *args_json, struct tool_result *result,
			      void *user_data)
{
	struct dynamic_tools_context *ctx = user_data;
	struct tool_entry *entry;
	struct dynamic_tool *dt;
	struct tool_checkpoint cp;
	cJSON *args = NULL;
	cJSON *id_item;
	char name[TOOL_NAME_MAX];
	char cp_id[16];
	char tool_dir[PATH_MAX];
	char cp_dir[PATH_MAX];
	char source_path[PATH_MAX];
	char *check_error = NULL;
	int rc;

	if (!ctx || !result)
		return -EINVAL;
	rc = parse_tool_name_arg(args_json, "tool_rollback", &args, name);
	if (rc < 0)
		return tool_result_error(result, "tool_failed",
					      "tool_rollback requires valid name");
	id_item = cJSON_GetObjectItem(args, "checkpoint_id");
	if (cJSON_IsString(id_item) && id_item->valuestring &&
	    strcmp(id_item->valuestring, "latest") != 0) {
		strncpy(cp_id, id_item->valuestring, sizeof(cp_id) - 1);
		cp_id[sizeof(cp_id) - 1] = '\0';
	} else {
		rc = create_context_dir(ctx, name, tool_dir, sizeof(tool_dir));
		if (rc == 0)
			rc = latest_checkpoint_id(tool_dir, cp_id, sizeof(cp_id));
		if (rc < 0) {
			cJSON_Delete(args);
			return tool_result_error(result, "tool_failed",
						      "checkpoint not found");
		}
	}
	cJSON_Delete(args);
	entry = tool_lookup(ctx->reg, name);
	if (!entry || !(entry->flags & TOOL_FLAG_DYNAMIC) || !entry->user_data)
		return tool_result_error(result, "tool_failed", "dynamic tool not found");
	dt = entry->user_data;
	strncpy(tool_dir, dt->dir, sizeof(tool_dir) - 1);
	tool_dir[sizeof(tool_dir) - 1] = '\0';
	rc = checkpoint_dir_path(tool_dir, cp_id, cp_dir, sizeof(cp_dir));
	if (rc == 0)
		rc = read_checkpoint_meta(cp_dir, &cp);
	if (rc < 0)
		return tool_result_error(result, "tool_failed", "checkpoint not found");
	rc = restore_tool_checkpoint(tool_dir, cp_dir, &cp);
	if (rc < 0)
		return tool_result_errorf(result, "tool_failed",
			"failed to restore checkpoint: %s", morph_strerror(rc));
	if (strcmp(cp.before_state, "absent") == 0) {
		rc = tool_unregister(ctx->reg, name);
		if (rc == -ENOENT)
			rc = 0;
		if (rc == 0)
			rc = reload_persistent_if_present(ctx, name);
		if (rc < 0)
			return tool_result_errorf(result, "tool_failed",
				"failed to unregister rolled back tool: %s",
				morph_strerror(rc));
		return tool_result_successf(result,
			"{\"name\":\"%s\",\"status\":\"rolled_back\","
			"\"checkpoint_id\":\"%s\",\"state\":\"absent\"}",
			name, cp.id);
	}
	rc = file_path_join(source_path, sizeof(source_path), tool_dir,
			    DYN_TOOL_SOURCE_FILE);
	if (rc == 0)
		rc = check_js_source(source_path, &check_error);
	if (rc < 0) {
		int out_rc;
		out_rc = tool_result_errorf(result, "tool_failed",
			"restored checkpoint failed validation: %s",
			check_error ? check_error : morph_strerror(rc));
		free(check_error);
		return out_rc;
	}
	free(check_error);
	rc = load_tool_from_dir(ctx->reg, ctx->tctx, ctx->cfg, tool_dir,
				TOOL_ORIGIN_DYNAMIC_SESSION);
	if (rc < 0)
		return tool_result_errorf(result, "tool_failed",
			"failed to register rolled back tool: %s",
			morph_strerror(rc));
	(void)remove_checkpoint_ids_from(tool_dir, checkpoint_id_value(cp.id));
	return tool_result_successf(result,
		"{\"name\":\"%s\",\"status\":\"rolled_back\","
		"\"checkpoint_id\":\"%s\",\"state\":\"present\"}",
		name, cp.id);
}

static void dynamic_tools_context_destroy(void *user_data)
{
	free(user_data);
}

int dynamic_tools_set_session_id(struct tool_registry *reg,
				 const char *session_id)
{
	struct tool_entry *entry;
	struct dynamic_tools_context *ctx;
	int rc;

	if (!reg)
		return -EINVAL;
	entry = tool_lookup(reg, "tool_create");
	if (!entry)
		return 0;
	ctx = entry->user_data;
	if (!ctx)
		return -EINVAL;
	snprintf(ctx->session_id, sizeof(ctx->session_id), "%s",
		 session_id ? session_id : "");
	if (!ctx->session_id[0])
		return 0;
	rc = dynamic_tools_load_session(reg, ctx->tctx, ctx->cfg,
					ctx->session_id);
	return rc < 0 ? rc : 0;
}

static int dynamic_tools_load_from_root(struct tool_registry *reg,
					struct tool_context *tctx,
					const struct config_dynamic_tools *cfg,
					const char *root,
					enum tool_origin origin)
{
	char **dirs = NULL;
	int count = 0;

	if (!reg || !cfg || !cfg->enabled || !root)
		return 0;
	if (!file_exists(root))
		return 0;
	if (file_list_dirs(root, &dirs, &count) != 0)
		return 0;
	for (int i = 0; i < count; i++) {
		char path[PATH_MAX];
		if (dirs[i][0] == '.')
			continue;
		if (file_path_join(path, sizeof(path), root, dirs[i]) != 0)
			continue;
		(void)load_tool_from_dir(reg, tctx, cfg, path, origin);
	}
	file_free_list(dirs, count);
	return 0;
}

static int dynamic_tools_load_session(struct tool_registry *reg,
				      struct tool_context *tctx,
				      const struct config_dynamic_tools *cfg,
				      const char *session_id)
{
	char session_root[PATH_MAX];
	int rc;

	if (!cfg || !cfg->enabled || !session_id || !*session_id)
		return 0;
	rc = file_path_join(session_root, sizeof(session_root),
			    cfg->session_dir, session_id);
	if (rc < 0)
		return rc;
	return dynamic_tools_load_from_root(reg, tctx, cfg, session_root,
					    TOOL_ORIGIN_DYNAMIC_SESSION);
}

int dynamic_tools_load_persistent(struct tool_registry *reg,
				  struct tool_context *tctx,
				  const struct config_dynamic_tools *cfg)
{
	if (!cfg)
		return 0;
	return dynamic_tools_load_from_root(reg, tctx, cfg,
					    cfg->persistent_dir,
					    TOOL_ORIGIN_DYNAMIC_PERSISTENT);
}

static const char *TOOL_CREATE_DESCRIPTION =
	"Create or update a dynamic JavaScript tool for this agent session. "
	"The source_js runs in embedded QuickJS. It must define global function "
	"run(args) or async function run(args), and return a JSON-serializable "
	"value. This is not Node.js: no node: imports, fs/http/https/"
	"child_process modules, process, Buffer, __dirname, require(), or npm "
	"packages. Use only the bounded morph host APIs. Image APIs: "
	"morph.image.metadata({input}), morph.image.resize({input,output,width,"
	"height}), morph.image.crop/extract({input,output,left,top,width,"
	"height}), morph.image.extend({input,output,top,bottom,left,right,"
	"background}), morph.image.rotate({input,output,angle}), "
	"morph.image.compose({input,output,overlays:[{input,left,top}]}), "
	"morph.image.convert({input,output}), morph.image.frame({input,output,"
	"style,caption,padding}), and morph.image.open(input) for the same "
	"finite image handle methods: metadata, resize, extract, extend, "
	"rotate, blur, sharpen, grayscale/greyscale, flatten, composite, png, "
	"jpeg/jpg, webp, toFile, toBuffer. Canvas APIs: "
	"morph.canvas.create({width,height}), morph.canvas.loadImage({input}), "
	"morph.canvas.toFile({canvas,output}), morph.canvas.toBuffer({canvas}); "
	"canvas handles support getContext(\"2d\"), fillRect, strokeRect, "
	"rect, beginPath, fill, stroke, save, restore, moveTo, lineTo, "
	"translate, arc, fillText, strokeText, drawImage, toFile, toBuffer. "
	"Canvas text supports ctx.font, ctx.textAlign (left, right, center, "
	"start, end), and ctx.textBaseline (top, hanging, middle, alphabetic, "
	"ideographic, bottom). "
	"Binary files use morph.fs.readFile(path) and morph.fs.writeFile(path, "
	"ArrayBufferOrUint8Array); text files use morph.fs.readText and "
	"morph.fs.writeText. Example image tool: async function run(args) { "
	"await morph.image.resize({input: args.input, output: args.output, "
	"width: 512}); return { output: args.output }; }. Example canvas tool: "
	"async function run(args) { const c = morph.canvas.create({width: 256, "
	"height: 128}); const ctx = c.getContext(\"2d\"); ctx.fillStyle = "
	"\"#ffffff\"; ctx.fillRect(0, 0, 256, 128); ctx.fillStyle = "
	"\"#111111\"; ctx.fillText(args.text, 20, 70); "
	"morph.canvas.toFile({canvas: c, output: args.output}); return { "
	"output: args.output }; }. "
	"Example wasm: const mod = await WebAssembly.instantiate(bytes, {}); "
	"const n = mod.instance.exports.add(1, 2). Available host APIs: "
	"morph.fs.readText(path), morph.fs.writeText(path, text), "
	"morph.env.get(name), morph.exec(command), morph.fetch(url). Host API "
	"capabilities are controlled by the active dynamic_tools profile, not "
	"by tool_create arguments. If a dynamic tool with the same name already "
	"exists, tool_create updates it; non-dynamic tools cannot be "
	"overwritten. Every successful create/update records a checkpoint; use "
	"tool_history, tool_diff, and tool_rollback to inspect or rewind dynamic "
	"tool changes.";

int dynamic_tools_init(struct tool_registry *reg, struct tool_context *tctx,
		       const struct config_dynamic_tools *cfg,
		       const char *session_id)
{
	struct dynamic_tools_context *ctx;
	int rc;

	if (!reg || !cfg)
		return -EINVAL;
	if (!cfg->enabled)
		return 0;
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->reg = reg;
	ctx->tctx = tctx;
	ctx->cfg = cfg;
	snprintf(ctx->session_id, sizeof(ctx->session_id), "%s",
		 session_id ? session_id : "");
	rc = tool_register(reg, &(struct tool_spec){
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "tool_create",
		.description = TOOL_CREATE_DESCRIPTION,
		.input_schema = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"New tool name: lowercase letters, digits, underscore only.\"},\"description\":{\"type\":\"string\",\"description\":\"Short model-facing description of what the new tool does.\"},\"input_schema\":{\"type\":[\"object\",\"string\"],\"description\":\"JSON Schema for the new tool's arguments.\"},\"output_schema\":{\"type\":[\"object\",\"string\"],\"description\":\"JSON Schema for the new tool's result data.\"},\"source_js\":{\"type\":\"string\",\"description\":\"JavaScript code defining global run(args).\"}},\"required\":[\"name\",\"input_schema\",\"output_schema\",\"source_js\"],\"additionalProperties\":false}",
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = tool_create_exec,
		.user_data = ctx,
		.user_data_destroy = dynamic_tools_context_destroy,
	});
	if (rc < 0) {
		free(ctx);
		return rc;
	}
	rc = tool_register(reg, &(struct tool_spec){ .origin = TOOL_ORIGIN_BUILTIN, .name = "tool_promote", .description = "Promote a session dynamic tool into the persistent dynamic tool directory.", .input_schema = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}", .output_schema = TOOL_OBJECT_OUTPUT_SCHEMA, .exec = tool_promote_exec, .user_data = ctx, .user_data_destroy = NULL });
	if (rc < 0)
		return rc;
	rc = tool_register(reg, &(struct tool_spec){ .origin = TOOL_ORIGIN_BUILTIN, .name = "tool_delete", .description = "Delete a dynamic tool. Session tools are removed from the current session directory; persistent dynamic tools are removed from the persistent dynamic tool directory. Built-in, MCP, and ext tools cannot be deleted.", .input_schema = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}", .output_schema = TOOL_OBJECT_OUTPUT_SCHEMA, .exec = tool_delete_exec, .user_data = ctx, .user_data_destroy = NULL });
	if (rc < 0)
		return rc;
	rc = tool_register(reg, &(struct tool_spec){ .origin = TOOL_ORIGIN_BUILTIN, .name = "tool_history", .description = "List checkpoints recorded for a session dynamic tool.", .input_schema = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"name\"]}", .output_schema = TOOL_OBJECT_OUTPUT_SCHEMA, .exec = tool_history_exec, .user_data = ctx, .user_data_destroy = NULL });
	if (rc < 0)
		return rc;
	rc = tool_register(reg, &(struct tool_spec){ .origin = TOOL_ORIGIN_BUILTIN, .name = "tool_diff", .description = "Show a unified diff between a dynamic tool checkpoint and the current tool files.", .input_schema = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"checkpoint_id\":{\"type\":\"string\",\"description\":\"Checkpoint id or latest.\"}},\"required\":[\"name\"]}", .output_schema = TOOL_OBJECT_OUTPUT_SCHEMA, .exec = tool_diff_exec, .user_data = ctx, .user_data_destroy = NULL });
	if (rc < 0)
		return rc;
	rc = tool_register(reg, &(struct tool_spec){ .origin = TOOL_ORIGIN_BUILTIN, .name = "tool_rollback", .description = "Roll back a session dynamic tool to a checkpoint. Rolling back a creation removes the tool.", .input_schema = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"checkpoint_id\":{\"type\":\"string\",\"description\":\"Checkpoint id or latest.\"}},\"required\":[\"name\"]}", .output_schema = TOOL_OBJECT_OUTPUT_SCHEMA, .exec = tool_rollback_exec, .user_data = ctx, .user_data_destroy = NULL });
	if (rc < 0)
		return rc;
	rc = dynamic_tools_load_persistent(reg, tctx, cfg);
	if (rc < 0)
		return rc;
	return dynamic_tools_load_session(reg, tctx, cfg, ctx->session_id);
}
