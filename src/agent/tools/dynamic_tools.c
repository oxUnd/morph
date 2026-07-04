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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
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
#define DYN_TOOL_OUTPUT_LIMIT (1024 * 1024)

struct dynamic_tool {
	char name[TOOL_NAME_MAX];
	char description[TOOL_DESC_MAX];
	char args_schema[TOOL_ARGS_SPEC_MAX];
	char source_path[PATH_MAX];
	char dir[PATH_MAX];
	char capabilities[DYNAMIC_TOOL_CAP_MAX][DYNAMIC_TOOL_CAP_LEN_MAX];
	int capabilities_count;
	const struct config_dynamic_tools *cfg;
	struct tool_context *tctx;
};

struct dynamic_tools_context {
	struct tool_registry *reg;
	struct tool_context *tctx;
	const struct config_dynamic_tools *cfg;
	char session_id[128];
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

static int cap_in_profile(const struct config_dynamic_tool_profile *profile,
			  const char *cap)
{
	if (!profile || !cap)
		return 0;
	for (int i = 0; i < profile->default_capabilities_count; i++) {
		if (strcmp(profile->default_capabilities[i], cap) == 0)
			return 1;
	}
	return 0;
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
	for (int i = 0; i < dt->capabilities_count &&
	     count < DYNAMIC_TOOL_CAP_MAX; i++) {
		if (!cap_in_profile(profile, dt->capabilities[i]))
			continue;
		strncpy(out[count], dt->capabilities[i],
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
	char caps[DYNAMIC_TOOL_CAP_MAX][DYNAMIC_TOOL_CAP_LEN_MAX];
	int caps_count = 0;
	char *cap_str;
	char *read_str;
	char *write_str;
	char *command_str;
	char *network_str;
	char timeout_buf[32];
	int rc;

	profile = active_profile(dt->cfg);
	rc = effective_caps(dt, caps, &caps_count);
	if (rc < 0)
		return rc;
	cap_str = join_caps(caps, caps_count);
	read_str = join_allow(profile->allowed_read_paths,
			      profile->allowed_read_paths_count);
	write_str = join_allow(profile->allowed_write_paths,
			       profile->allowed_write_paths_count);
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
	setenv("MORPH_DYNAMIC_CAPS", cap_str, 1);
	setenv("MORPH_DYNAMIC_ALLOWED_READ", read_str, 1);
	setenv("MORPH_DYNAMIC_ALLOWED_WRITE", write_str, 1);
	setenv("MORPH_DYNAMIC_ALLOWED_COMMANDS", command_str, 1);
	setenv("MORPH_DYNAMIC_ALLOWED_NETWORK", network_str, 1);
	setenv("MORPH_DYNAMIC_TIMEOUT", timeout_buf, 1);
	free(cap_str);
	free(read_str);
	free(write_str);
	free(command_str);
	free(network_str);
	return 0;
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
		close(stdin_pipe[1]);
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);
		dup2(stdin_pipe[0], STDIN_FILENO);
		dup2(stdout_pipe[1], STDOUT_FILENO);
		dup2(stderr_pipe[1], STDERR_FILENO);
		close(stdin_pipe[0]);
		close(stdout_pipe[1]);
		close(stderr_pipe[1]);
		(void)set_child_env(dt);
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
		return tool_result_json_error(result,
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
		(void)tool_result_json_error(result, "dynamic tool timed out");
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
		(void)tool_result_json_error(result,
					     "dynamic tool process terminated");
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
			(void)tool_result_json_error(result,
						     "invalid dynamic tool response");
			return prc;
		}
		if (jr.has_error) {
			char msg[1024];
			snprintf(msg, sizeof(msg), "dynamic tool error: %s",
				 jr.error_message ? jr.error_message : "unknown");
			jsonrpc_response_free(&jr);
			morph_buf_cleanup(&err);
			(void)tool_result_json_error(result, msg);
			return -EIO;
		}
		rc = tool_result_take_json(result,
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
		strncpy(existing->desc.desc, dt->description,
			sizeof(existing->desc.desc) - 1);
		strncpy(existing->desc.args_spec, dt->args_schema,
			sizeof(existing->desc.args_spec) - 1);
		existing->exec = dynamic_tool_exec;
		existing->user_data = dt;
		existing->user_data_destroy = dynamic_tool_destroy;
		existing->flags |= TOOL_FLAG_DYNAMIC;
		log_dbg("dynamic tool replaced: %s", dt->name);
		return 1;
	}
	rc = tool_register(reg, dt->name, dt->description, dt->args_schema,
			   dynamic_tool_exec, dt, dynamic_tool_destroy);
	if (rc < 0)
		return rc;
	{
		struct tool_entry *entry = tool_lookup(reg, dt->name);
		if (entry)
			entry->flags |= TOOL_FLAG_DYNAMIC;
	}
	return 0;
}

static int parse_caps(cJSON *arr, struct dynamic_tool *dt,
		      const struct config_dynamic_tool_profile *profile)
{
	if (!dt || !profile)
		return -EINVAL;
	dt->capabilities_count = 0;
	if (cJSON_IsArray(arr)) {
		cJSON *item;
		cJSON_ArrayForEach(item, arr) {
			if (!cJSON_IsString(item) || !item->valuestring)
				continue;
			if (dt->capabilities_count >= DYNAMIC_TOOL_CAP_MAX)
				break;
			strncpy(dt->capabilities[dt->capabilities_count],
				item->valuestring, DYNAMIC_TOOL_CAP_LEN_MAX - 1);
			dt->capabilities_count++;
		}
		return 0;
	}
	for (int i = 0; i < profile->default_capabilities_count &&
	     i < DYNAMIC_TOOL_CAP_MAX; i++) {
		strncpy(dt->capabilities[i], profile->default_capabilities[i],
			DYNAMIC_TOOL_CAP_LEN_MAX - 1);
		dt->capabilities_count++;
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
			"name, source_js, optional description, args_schema, "
			"and capabilities.\n");
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
	return tool_result_json_errorf(result,
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
	cJSON *caps = NULL;
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
	cJSON_AddStringToObject(meta, "args_schema", dt->args_schema);
	caps = cJSON_AddArrayToObject(meta, "capabilities");
	if (!caps) {
		cJSON_Delete(meta);
		return -ENOMEM;
	}
	for (int i = 0; i < dt->capabilities_count; i++)
		cJSON_AddItemToArray(caps,
				     cJSON_CreateString(dt->capabilities[i]));
	meta_json = cJSON_PrintUnformatted(meta);
	cJSON_Delete(meta);
	if (!meta_json)
		return -ENOMEM;
	rc = file_write_all(meta_path, meta_json, strlen(meta_json));
	free(meta_json);
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
			      const char *dir)
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
		cJSON *args = cJSON_GetObjectItem(meta, "args_schema");
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
		schema = schema_to_string(args);
		if (!schema) {
			cJSON_Delete(meta);
			free(dt);
			return -ENOMEM;
		}
		strncpy(dt->args_schema, schema, sizeof(dt->args_schema) - 1);
		free(schema);
	}
	strncpy(dt->source_path, source_path, sizeof(dt->source_path) - 1);
	strncpy(dt->dir, dir, sizeof(dt->dir) - 1);
	dt->cfg = cfg;
	dt->tctx = tctx;
	parse_caps(cJSON_GetObjectItem(meta, "capabilities"), dt,
		   active_profile(cfg));
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

	rc = file_path_join(session_root, sizeof(session_root),
			    ctx->cfg->session_dir,
			    ctx->session_id[0] ? ctx->session_id : "default");
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
	char *check_error = NULL;
	struct dynamic_tool *dt = NULL;
	const char *stage = "init";
	int rc;

	if (!ctx || !result)
		return -EINVAL;
	if (!ctx->cfg || !ctx->cfg->enabled)
		return tool_result_json_error(result, "dynamic tools disabled");
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
				"\"args_schema\":{\"type\":\"object\","
				"\"properties\":{}},"
				"\"source_js\":\"async function run(args) { "
				"return { ok: true }; }\"}",
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
		struct tool_entry *existing =
			tool_lookup(ctx->reg, name_item->valuestring);
		if (existing && !(existing->flags & TOOL_FLAG_DYNAMIC)) {
			cJSON_Delete(root);
			return tool_create_fail(result, stage, -EEXIST,
						"tool name already exists and is "
						"not a dynamic tool");
		}
	}
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
	stage = "validate_args_schema";
	schema = schema_to_string(cJSON_GetObjectItem(root, "args_schema"));
	if (!schema) {
		cJSON_Delete(root);
		return tool_create_fail(result, stage, -ENOMEM,
					"failed to serialize args_schema");
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
					"failed to allocate args_schema "
					"diagnostic buffer");
			}
			(void)morph_buf_printf(&detail,
				"invalid args_schema JSON near: %.80s\n"
				"args_schema must be a JSON Schema object or a "
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
	stage = "allocate_tool";
	dt = calloc(1, sizeof(*dt));
	if (!dt) {
		free(schema);
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
	strncpy(dt->args_schema, schema, sizeof(dt->args_schema) - 1);
	free(schema);
	dt->cfg = ctx->cfg;
	dt->tctx = ctx->tctx;
	stage = "create_tool_dir";
	rc = create_context_dir(ctx, dt->name, dt->dir, sizeof(dt->dir));
	if (rc == 0) {
		stage = "build_source_path";
		rc = file_path_join(dt->source_path, sizeof(dt->source_path),
				    dt->dir, DYN_TOOL_SOURCE_FILE);
	}
	if (rc == 0) {
		stage = "parse_capabilities";
		rc = parse_caps(cJSON_GetObjectItem(root, "capabilities"), dt,
				active_profile(ctx->cfg));
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
	}
	cJSON_Delete(root);
	if (rc < 0) {
		if (check_error) {
			char *formatted = format_check_error(dt ? dt->source_path :
							     NULL, check_error);
			int out_rc;
			free(dt);
			if (formatted) {
				out_rc = tool_result_json_error(result,
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
	return tool_result_printf(result,
				  "{\"name\":\"%s\",\"status\":\"%s\"}",
				  dt->name,
				  rc > 0 ? "updated" : "registered");
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
		return tool_result_json_error(result, "invalid JSON");
	name_item = cJSON_GetObjectItem(root, "name");
	if (!cJSON_IsString(name_item) || !valid_tool_name(name_item->valuestring)) {
		cJSON_Delete(root);
		return tool_result_json_error(result,
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
			return tool_result_json_error(result,
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
	cJSON_Delete(root);
	if (rc < 0)
		return tool_result_json_errorf(result,
			"failed to promote dynamic tool: %s", morph_strerror(rc));
	return tool_result_printf(result,
				  "{\"name\":\"%s\",\"status\":\"promoted\"}",
				  name);
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

	if (!reg)
		return -EINVAL;
	entry = tool_lookup(reg, "tool_create");
	if (!entry)
		return 0;
	ctx = entry->user_data;
	if (!ctx)
		return -EINVAL;
	snprintf(ctx->session_id, sizeof(ctx->session_id), "%s",
		 session_id && *session_id ? session_id : "default");
	return 0;
}

int dynamic_tools_load_persistent(struct tool_registry *reg,
				  struct tool_context *tctx,
				  const struct config_dynamic_tools *cfg)
{
	char **dirs = NULL;
	int count = 0;

	if (!reg || !cfg || !cfg->enabled)
		return 0;
	if (!file_exists(cfg->persistent_dir))
		return 0;
	if (file_list_dirs(cfg->persistent_dir, &dirs, &count) != 0)
		return 0;
	for (int i = 0; i < count; i++) {
		char path[PATH_MAX];
		if (dirs[i][0] == '.')
			continue;
		if (file_path_join(path, sizeof(path),
				   cfg->persistent_dir, dirs[i]) != 0)
			continue;
		(void)load_tool_from_dir(reg, tctx, cfg, path);
	}
	file_free_list(dirs, count);
	return 0;
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
	"morph.env.get(name), morph.exec(command), morph.fetch(url). Request "
	"capabilities when using host APIs: fs_read, fs_write, env, "
	"shell/process, network, image, wasm. If a dynamic tool with the same "
	"name already exists, tool_create updates it; non-dynamic tools cannot "
	"be overwritten.";

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
		 session_id && *session_id ? session_id : "default");
	rc = tool_register(reg, "tool_create", TOOL_CREATE_DESCRIPTION,
			   "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"New tool name: lowercase letters, digits, underscore only.\"},\"description\":{\"type\":\"string\",\"description\":\"Short model-facing description of what the new tool does.\"},\"args_schema\":{\"type\":[\"object\",\"string\"],\"description\":\"JSON Schema for the new tool's arguments.\"},\"source_js\":{\"type\":\"string\",\"description\":\"JavaScript code defining global run(args).\"},\"capabilities\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional host capabilities: fs_read, fs_write, network, process, env, mcp, model, shell, image, wasm.\"}},\"required\":[\"name\",\"source_js\"]}",
			   tool_create_exec, ctx, dynamic_tools_context_destroy);
	if (rc < 0) {
		free(ctx);
		return rc;
	}
	rc = tool_register(reg, "tool_promote",
			   "Promote a session dynamic tool into the persistent dynamic tool directory.",
			   "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}",
			   tool_promote_exec, ctx, NULL);
	if (rc < 0)
		return rc;
	return dynamic_tools_load_persistent(reg, tctx, cfg);
}
