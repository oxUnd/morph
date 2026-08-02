#include "bash_exec.h"
#include "agent/tool_context.h"
#include "sandbox.h"
#include "util/log.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/file.h"
#include "util/bash_parse.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BASH_EXEC_MAX_OUTPUT (256 * 1024)

static int bash_exec_default_timeout = 60;

static const char *const local_network_env[] = {
	"http_proxy", "https_proxy", "all_proxy", "no_proxy",
	"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY",
	"SSL_CERT_FILE", "SSL_CERT_DIR", "CURL_CA_BUNDLE"
};

void bash_exec_set_default_timeout(int seconds)
{
	if (seconds > 0)
		bash_exec_default_timeout = seconds;
}

static const char *const legacy_blocked_commands[] = {
	"rm", "rmdir", "mkfs", "dd", "shutdown", "reboot", "poweroff",
	"halt", "init", "mv", "cp", "chmod", "chown", "chgrp", "chattr",
	"useradd", "userdel", "usermod", "groupadd", "groupdel", "passwd",
	"crontab", "systemctl", "service", "launchctl", "kill", "killall",
	"pkill", "ssh", "scp", "sftp", "rsync", "mount", "umount",
	"fdisk", "parted", "diskutil", "sysctl", "iptables", "ip6tables",
	"defaults", NULL
};

static int legacy_command_is_blocked(const char *command)
{
	struct bash_parse_result analysis;
	struct bash_parse_command *parsed;
	int blocked = 0;

	if (bash_parse_analyze(command, &analysis) != 0)
		return 1;
	if (analysis.has_error || analysis.commands.nelts == 0)
		blocked = 1;
	morph_array_foreach(parsed, &analysis.commands,
			    struct bash_parse_command) {
		for (const char *const *name = legacy_blocked_commands;
		     *name; name++) {
			size_t len = strlen(*name);

			if (strcmp(parsed->name, *name) == 0 ||
			    (strncmp(parsed->name, *name, len) == 0 &&
			     parsed->name[len] == '.')) {
				blocked = 1;
				break;
			}
		}
		if (blocked)
			break;
	}
	bash_parse_result_cleanup(&analysis);
	return blocked;
}

static int command_has_compound_syntax(const char *command)
{
	struct bash_parse_result analysis;
	int compound;

	if (!command || bash_parse_analyze(command, &analysis) != 0)
		return 1;
	compound = analysis.has_error || analysis.is_compound;
	bash_parse_result_cleanup(&analysis);
	return compound;
}

static int json_paths_allow(cJSON *paths, const char *path)
{
	cJSON *item;

	if (!cJSON_IsArray(paths) || !path)
		return 0;
	cJSON_ArrayForEach(item, paths) {
		if (cJSON_IsString(item) && item->valuestring &&
		    path_is_within(path, item->valuestring))
			return 1;
	}
	return 0;
}

static int add_allowed_path(char **allowed, int *count, const char *path)
{
	if (!allowed || !count || !path || !*path)
		return 0;
	for (int i = 0; i < *count; i++) {
		if (strcmp(allowed[i], path) == 0)
			return 0;
	}
	allowed[(*count)++] = (char *)path;
	return 1;
}

static int buf_append(morph_buf_t *b, const char *s, size_t n)
{
	if (b->len >= BASH_EXEC_MAX_OUTPUT)
		return 0;
	if (b->len + n > BASH_EXEC_MAX_OUTPUT)
		n = BASH_EXEC_MAX_OUTPUT - b->len;
	return morph_buf_append(b, s, n);
}

static int read_pipes_with_timeout(int out_fd, int err_fd,
				   morph_buf_t *out_buf, morph_buf_t *err_buf,
				   int timeout_seconds, int *timed_out)
{
	int max_fd = (out_fd > err_fd) ? out_fd : err_fd;
	int out_open = 1, err_open = 1;
	time_t deadline = time(NULL) + timeout_seconds;

	fcntl(out_fd, F_SETFL, O_NONBLOCK);
	fcntl(err_fd, F_SETFL, O_NONBLOCK);

	while (out_open || err_open) {
		fd_set rfds;
		FD_ZERO(&rfds);
		if (out_open)
			FD_SET(out_fd, &rfds);
		if (err_open)
			FD_SET(err_fd, &rfds);

		time_t now = time(NULL);
		if (now >= deadline) {
			*timed_out = 1;
			return 0;
		}
		struct timeval tv;
		tv.tv_sec = deadline - now;
		tv.tv_usec = 0;

		int rc = select(max_fd + 1, &rfds, NULL, NULL, &tv);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			MORPH_RETURN_ERRNO();
		}
		if (rc == 0) {
			*timed_out = 1;
			return 0;
		}

		char tmp[BUFSIZ];
		if (out_open && FD_ISSET(out_fd, &rfds)) {
			ssize_t n = read(out_fd, tmp, sizeof(tmp));
			if (n > 0)
				buf_append(out_buf, tmp, (size_t)n);
			else if (n == 0)
				out_open = 0;
			else if (errno != EAGAIN && errno != EINTR)
				out_open = 0;
		}
		if (err_open && FD_ISSET(err_fd, &rfds)) {
			ssize_t n = read(err_fd, tmp, sizeof(tmp));
			if (n > 0)
				buf_append(err_buf, tmp, (size_t)n);
			else if (n == 0)
				err_open = 0;
			else if (errno != EAGAIN && errno != EINTR)
				err_open = 0;
		}
	}
	return 0;
}

static int bash_exec_run_legacy(const char *args_json,
				struct tool_result *result, void *user_data)
{
	struct tool_context *tctx = user_data;
	if (!result)
		return -EINVAL;

	cJSON *root = args_json ? cJSON_Parse(args_json) : NULL;
	const char *command = NULL;
	const char *cwd = NULL;
	char principal[256];
	cJSON *write_paths = NULL;
	cJSON *approved_write_paths = NULL;
	cJSON *approved_auto_paths = NULL;
	struct tool_directory_capability auto_dirs[TOOL_CONTEXT_CLI_DIR_MAX];
	int auto_dir_count = 0;
	int timeout = bash_exec_default_timeout;
	if (root) {
		cJSON *c = cJSON_GetObjectItem(root, "command");
		if (cJSON_IsString(c) && c->valuestring)
			command = c->valuestring;
		cJSON *w = cJSON_GetObjectItem(root, "cwd");
		if (cJSON_IsString(w) && w->valuestring)
			cwd = w->valuestring;
		cJSON *t = cJSON_GetObjectItem(root, "timeout_seconds");
		if (cJSON_IsNumber(t) && t->valuedouble > 0)
			timeout = (int)t->valuedouble;
		write_paths = cJSON_GetObjectItem(root, "write_paths");
	}

	if (!command || !*command) {
		if (root)
			cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"missing 'command' parameter. "
			"Usage: bash_exec({\\\"command\\\": \\\"ls -la\\\"})\"}"));
		return -EINVAL;
	}
	if (legacy_command_is_blocked(command)) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"command blocked for safety\"}"));
		MORPH_RETURN(-EPERM);
	}

	if (write_paths && !cJSON_IsArray(write_paths)) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"write_paths must be an array "
			"of directory paths\"}"));
		return -EINVAL;
	}
	if (cJSON_IsArray(write_paths) &&
	    command_has_compound_syntax(command)) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"write_paths requires a single simple "
			"command without shell operators\"}"));
		return -EPERM;
	}
	if (tool_context_command_principal(command, principal,
					   sizeof(principal)) != 0)
		snprintf(principal, sizeof(principal), "%s", "shell");

	if (tctx) {
		enum tool_operation_verdict command_verdict = TOOL_OP_DENY;
		struct tool_operation op = {
			.kind = TOOL_OP_COMMAND,
			.tool_name = "bash_exec",
			.principal = principal,
			.action = command,
			.target = NULL,
			.scope = cwd,
			.details_json = args_json,
		};
		int rc;

		if (!command_has_compound_syntax(command)) {
			auto_dir_count = tool_context_discover_cli_dirs(
				tctx, command, auto_dirs,
				TOOL_CONTEXT_CLI_DIR_MAX);
			if (auto_dir_count < 0) {
				cJSON_Delete(root);
				return auto_dir_count;
			}
			op.directories = auto_dirs;
			op.directories_count = auto_dir_count;
		}
		rc = tool_context_check_operation_verdict(
			tctx, &op, &command_verdict);
		if (rc < 0) {
			if (root)
				cJSON_Delete(root);
			if (command_verdict != TOOL_OP_DENY)
				(void)tool_result_success_json_text(result, strdup(
					"{\"error\":\"approved CLI "
					"directory could not be prepared\"}"));
			else if (rc == -EACCES)
				(void)tool_result_success_json_text(result, strdup(
					"{\"error\":\"command execution denied "
					"by user\"}"));
			else
				(void)tool_result_success_json_text(result, strdup(
					"{\"error\":\"command or cwd is not "
					"allowed by policy and no interactive "
					"approval is available\"}"));
			return -EPERM;
		}
		if (auto_dir_count > 0 &&
		    command_verdict != TOOL_OP_DENY) {
			approved_auto_paths = cJSON_CreateArray();
			if (!approved_auto_paths) {
				cJSON_Delete(root);
				return -ENOMEM;
			}
			cJSON_AddItemToObject(root, "_approved_auto_paths",
					      approved_auto_paths);
			for (int i = 0; i < auto_dir_count; i++) {
				char *resolved;

				resolved = file_resolve_path(
					auto_dirs[i].path);
				if (!resolved) {
					cJSON_Delete(root);
					(void)tool_result_success_json_text(
						result, strdup(
							"{\"error\":\"approved "
							"CLI directory could not "
							"be prepared\"}"));
					return -ENOMEM;
				}
				cJSON_AddItemToArray(
					approved_auto_paths,
					cJSON_CreateString(resolved));
				free(resolved);
			}
		}
		if (cJSON_IsArray(write_paths)) {
			cJSON *path;

			approved_write_paths = cJSON_CreateArray();
			if (!approved_write_paths) {
				cJSON_Delete(root);
				return -ENOMEM;
			}
			cJSON_AddItemToObject(root, "_approved_write_paths",
					      approved_write_paths);
			cJSON_ArrayForEach(path, write_paths) {
				char resolved[PATH_MAX];
				int path_rc;
				char *expanded;
				char *canonical;

				if (!cJSON_IsString(path) || !path->valuestring) {
					cJSON_Delete(root);
					(void)tool_result_success_json_text(
						result, strdup(
						"{\"error\":\"write_paths must "
						"contain only strings\"}"));
					return -EINVAL;
				}
				expanded = file_expand_path(path->valuestring);
				canonical = expanded ?
					file_resolve_path(expanded) : NULL;
				free(expanded);
				if (canonical &&
				    file_path_is_absolute(canonical) &&
				    json_paths_allow(approved_auto_paths,
						     canonical)) {
					snprintf(resolved, sizeof(resolved),
						 "%s", canonical);
					path_rc = 0;
				} else {
					path_rc =
						tool_context_request_write_access(
							tctx, principal, command,
							path->valuestring,
							resolved,
							sizeof(resolved));
				}
				free(canonical);
				if (path_rc < 0) {
					cJSON *denied = cJSON_CreateObject();
					cJSON_AddStringToObject(
						denied, "error",
						path_rc == -EACCES ?
						"filesystem permission denied "
						"by user" :
						"filesystem permission requires "
						"interactive approval");
					cJSON_AddStringToObject(
						denied, "principal", principal);
					cJSON_AddStringToObject(
						denied, "capability", "write");
					cJSON_AddStringToObject(
						denied, "target", resolved);
					char *text =
						cJSON_PrintUnformatted(denied);
					cJSON_Delete(denied);
					cJSON_Delete(root);
					(void)tool_result_success_json_text(
						result, text ? text :
						strdup("{\"error\":"
						       "\"permission denied\"}"));
					return -EPERM;
				}
				cJSON_AddItemToArray(
					approved_write_paths,
					cJSON_CreateString(resolved));
			}
		}
	}

	const char *effective_cwd = cwd;
	char workdir_buf[PATH_MAX];
	if ((!effective_cwd || !*effective_cwd) && tctx) {
		const char *wd = tool_context_workdir(tctx);
		if (wd && *wd) {
			strncpy(workdir_buf, wd, sizeof(workdir_buf) - 1);
			workdir_buf[sizeof(workdir_buf) - 1] = '\0';
			effective_cwd = workdir_buf;
		}
	}

	int out_pipe[2], err_pipe[2];
	if (pipe(out_pipe) < 0) {
		int err = errno;
		if (root)
			cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"pipe() failed\"}"));
		MORPH_RETURN(-err);
	}
	if (pipe(err_pipe) < 0) {
		int err = errno;
		close(out_pipe[0]);
		close(out_pipe[1]);
		if (root)
			cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"pipe() failed\"}"));
		MORPH_RETURN(-err);
	}

	log_info("bash_exec: running '%s' (cwd=%s, timeout=%ds)",
		 command, effective_cwd ? effective_cwd : ".", timeout);

	pid_t pid = fork();
	if (pid < 0) {
		int err = errno;
		close(out_pipe[0]); close(out_pipe[1]);
		close(err_pipe[0]); close(err_pipe[1]);
		if (root)
			cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"fork() failed\"}"));
		MORPH_RETURN(-err);
	}

	if (pid == 0) {
		char resolved_cwd[PATH_MAX];
		const char *sandbox_cwd = effective_cwd;
		const char **grants = NULL;
		char **allowed = NULL;
		int grant_count = 0;
		int auto_count = approved_auto_paths ?
			cJSON_GetArraySize(approved_auto_paths) : 0;
		int requested_count = approved_write_paths ?
			cJSON_GetArraySize(approved_write_paths) : 0;
		int grant_capacity = tctx ?
			(int)(tctx->scoped_grants.nelts +
			      tctx->persistent_grants.nelts) : 0;
		int allowed_count = 0;

		close(out_pipe[0]);
		close(err_pipe[0]);
		dup2(out_pipe[1], STDOUT_FILENO);
		dup2(err_pipe[1], STDERR_FILENO);
		close(out_pipe[1]);
		close(err_pipe[1]);

		int devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			close(devnull);
		}

		if (effective_cwd && *effective_cwd) {
			if (!realpath(effective_cwd, resolved_cwd) ||
			    chdir(resolved_cwd) != 0) {
				fprintf(stderr,
					"bash_exec: chdir(%s) failed: %s\n",
					effective_cwd, strerror(errno));
				_exit(126);
			}
			sandbox_cwd = resolved_cwd;
		}

		struct sandbox_config sb;
		int sb_rc;
		memset(&sb, 0, sizeof(sb));
		sb.permissions = EXT_PERM_EXEC | EXT_PERM_NETWORK;
		allowed = calloc((size_t)(2 + auto_count + requested_count +
					 grant_capacity), sizeof(*allowed));
		if (!allowed)
			_exit(126);
		if (grant_capacity > 0) {
			grants = calloc((size_t)grant_capacity,
					sizeof(*grants));
			if (!grants)
				_exit(126);
			grant_count = tool_context_collect_write_grants(
				tctx, principal, grants, grant_capacity);
		}

		if (effective_cwd && *effective_cwd)
			sb.permissions |= EXT_PERM_FILESYS;
		sb.max_memory_mb = 512;
		sb.max_cpu_seconds = timeout;

		if (sandbox_cwd && *sandbox_cwd) {
			add_allowed_path(allowed, &allowed_count, sandbox_cwd);
		}
		if (tctx) {
			const char *od = tool_context_output_dir(tctx);
			if (od && *od) {
				char resolved_od[PATH_MAX];
				if (realpath(od, resolved_od)) {
					if (!sandbox_cwd ||
					    strcmp(resolved_od, sandbox_cwd) != 0)
						add_allowed_path(
							allowed, &allowed_count,
							resolved_od);
				}
			}
		}
		for (int i = 0; i < auto_count; i++) {
			cJSON *path = cJSON_GetArrayItem(
				approved_auto_paths, i);
			if (cJSON_IsString(path) && path->valuestring)
				add_allowed_path(allowed, &allowed_count,
						 path->valuestring);
		}
		for (int i = 0; i < requested_count; i++) {
			cJSON *path = cJSON_GetArrayItem(
				approved_write_paths, i);
			if (cJSON_IsString(path) && path->valuestring)
				add_allowed_path(allowed, &allowed_count,
						 path->valuestring);
		}
		for (int i = 0; i < grant_count; i++)
			add_allowed_path(allowed, &allowed_count, grants[i]);
		if (auto_count > 0 || requested_count > 0 ||
		    grant_count > 0)
			sb.permissions |= EXT_PERM_FILESYS;
		sb.allowed_paths = allowed;
		sb.allowed_paths_count = allowed_count;
		sb_rc = sandbox_enter(&sb);
		if (sb_rc < 0) {
			fprintf(stderr,
				"bash_exec: sandbox initialization failed: %s\n",
				morph_strerror(sb_rc));
			_exit(126);
		}

		execl("/bin/sh", "sh", "-c", command, (char *)NULL);
		fprintf(stderr, "bash_exec: execl failed: %s\n",
			strerror(errno));
		_exit(127);
	}

	close(out_pipe[1]);
	close(err_pipe[1]);

	morph_buf_t out_buf;
	morph_buf_t err_buf;
	int buf_rc = morph_buf_init(&out_buf, 4096);
	if (buf_rc == 0)
		buf_rc = morph_buf_init(&err_buf, 4096);
	if (buf_rc != 0) {
		morph_buf_cleanup(&out_buf);
		close(out_pipe[0]);
		close(err_pipe[0]);
		if (root)
			cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"buffer allocation failed\"}"));
		return buf_rc;
	}

	int timed_out = 0;
	read_pipes_with_timeout(out_pipe[0], err_pipe[0],
				&out_buf, &err_buf, timeout, &timed_out);
	close(out_pipe[0]);
	close(err_pipe[0]);

	int status = 0;
	if (timed_out) {
		log_warn("bash_exec: timeout after %ds, killing pid %d",
			 timeout, pid);
		kill(pid, SIGKILL);
	}
	waitpid(pid, &status, 0);

	int exit_code = -1;
	const char *signal_name = NULL;
	if (WIFEXITED(status))
		exit_code = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		signal_name = strsignal(WTERMSIG(status));

	cJSON *out = cJSON_CreateObject();
	cJSON_AddStringToObject(out, "command", command);
	if (cwd)
		cJSON_AddStringToObject(out, "cwd", cwd);
	cJSON_AddNumberToObject(out, "exit_code", exit_code);
	cJSON_AddBoolToObject(out, "timed_out", timed_out);
	if (signal_name)
		cJSON_AddStringToObject(out, "signal", signal_name);
	cJSON_AddStringToObject(out, "stdout",
				out_buf.data ? out_buf.data : "");
	cJSON_AddStringToObject(out, "stderr",
				err_buf.data ? err_buf.data : "");
	cJSON_AddBoolToObject(out, "stdout_truncated",
			      out_buf.len >= BASH_EXEC_MAX_OUTPUT);
	cJSON_AddBoolToObject(out, "stderr_truncated",
			      err_buf.len >= BASH_EXEC_MAX_OUTPUT);
	if (timed_out)
		cJSON_AddStringToObject(out, "error",
					"command timed out");
	else if (exit_code != 0)
		cJSON_AddStringToObject(out, "error",
					"command exited with non-zero status");

	char *str = cJSON_PrintUnformatted(out);
	cJSON_Delete(out);
	morph_buf_cleanup(&out_buf);
	morph_buf_cleanup(&err_buf);
	if (root)
		cJSON_Delete(root);

	(void)tool_result_success_json_text(result, str ? str : strdup("{}"));
	return 0;
}

static int path_in_configured_dirs(
	const char *path,
	char dirs[][TOOL_CONTEXT_ALLOW_PATH_MAX], int count)
{
	for (int i = 0; i < count; i++)
		if (path_is_within(path, dirs[i]))
			return 1;
	return 0;
}

static int path_in_local_defaults(struct tool_context *tctx,
				  const char *path)
{
	const char *workdir = tool_context_workdir(tctx);
	const char *output = tool_context_output_dir(tctx);

	return (workdir && *workdir && path_is_within(path, workdir)) ||
		(output && *output && path_is_within(path, output)) ||
		path_is_within(path, "/tmp");
}

static int prepare_policy_paths(struct tool_context *tctx,
				const char *principal,
				const char *command, cJSON *paths,
				enum tool_path_op operation,
				cJSON **approved_out,
				struct tool_result *result)
{
	cJSON *approved;
	cJSON *path;

	if (!paths) {
		*approved_out = NULL;
		return 0;
	}
	if (!cJSON_IsArray(paths)) {
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"capability paths must be arrays\"}"));
		MORPH_RETURN(-EINVAL);
	}
	approved = cJSON_CreateArray();
	if (!approved)
		MORPH_RETURN(-ENOMEM);
	*approved_out = approved;
	cJSON_ArrayForEach(path, paths) {
		char resolved[PATH_MAX];
		char *canonical;
		int rc = 0;

		if (!cJSON_IsString(path) || !path->valuestring ||
		    !file_path_is_absolute(path->valuestring)) {
			(void)tool_result_success_json_text(result, strdup(
				"{\"error\":\"capability paths must contain "
				"absolute directory paths\"}"));
			MORPH_RETURN(-EINVAL);
		}
		canonical = file_resolve_path(path->valuestring);
		if (!canonical)
			MORPH_RETURN(-ENOMEM);
		snprintf(resolved, sizeof(resolved), "%s", canonical);
		free(canonical);
		if (tctx->bash_exec_local_mode) {
			if (!path_in_local_defaults(tctx, resolved)) {
				if (operation == TOOL_PATH_WRITE)
					rc = tool_context_request_write_access(
						tctx, principal, command, resolved,
						resolved, sizeof(resolved));
				else
					rc = tool_context_request_delete_access(
						tctx, principal, command, resolved,
						resolved, sizeof(resolved));
			}
		} else {
			char (*dirs)[TOOL_CONTEXT_ALLOW_PATH_MAX];
			int count;

			if (operation == TOOL_PATH_WRITE) {
				dirs = tctx->bash_exec_server_write_dirs;
				count = tctx->bash_exec_server_write_dirs_count;
			} else {
				dirs = tctx->bash_exec_server_delete_dirs;
				count = tctx->bash_exec_server_delete_dirs_count;
			}
			if (!path_in_configured_dirs(resolved, dirs, count))
				rc = -EPERM;
		}
		if (rc != 0) {
			cJSON *error = cJSON_CreateObject();
			char *text;

			cJSON_AddStringToObject(error, "error",
				rc == -EACCES ? "permission denied by user" :
				"path is outside the configured sandbox policy");
			cJSON_AddStringToObject(error, "capability",
				operation == TOOL_PATH_WRITE ? "write" : "delete");
			cJSON_AddStringToObject(error, "target", resolved);
			text = cJSON_PrintUnformatted(error);
			cJSON_Delete(error);
			(void)tool_result_success_json_text(result,
				text ? text : strdup("{\"error\":\"permission denied\"}"));
			return rc;
		}
		cJSON_AddItemToArray(approved, cJSON_CreateString(resolved));
	}
	return 0;
}

static void add_json_paths(char **paths, int *count, cJSON *array)
{
	int n = array ? cJSON_GetArraySize(array) : 0;

	for (int i = 0; i < n; i++) {
		cJSON *item = cJSON_GetArrayItem(array, i);

		if (cJSON_IsString(item) && item->valuestring)
			add_allowed_path(paths, count, item->valuestring);
	}
}

static int bash_exec_run_policy(const char *args_json,
				struct tool_result *result,
				struct tool_context *tctx)
{
	cJSON *root = args_json ? cJSON_Parse(args_json) : NULL;
	cJSON *write_paths;
	cJSON *delete_paths;
	cJSON *approved_write = NULL;
	cJSON *approved_delete = NULL;
	const char *command = NULL;
	const char *cwd = NULL;
	const char *effective_cwd;
	char workdir_buf[PATH_MAX];
	char principal[256];
	int timeout = bash_exec_default_timeout;
	int rc;
	int out_pipe[2];
	int err_pipe[2];
	pid_t pid;

	if (!root)
		MORPH_RETURN(-EINVAL);
	{
		cJSON *item = cJSON_GetObjectItem(root, "command");

		if (cJSON_IsString(item) && item->valuestring)
			command = item->valuestring;
		item = cJSON_GetObjectItem(root, "cwd");
		if (cJSON_IsString(item) && item->valuestring)
			cwd = item->valuestring;
		item = cJSON_GetObjectItem(root, "timeout_seconds");
		if (cJSON_IsNumber(item) && item->valuedouble > 0)
			timeout = (int)item->valuedouble;
	}
	if (!command || !*command) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"missing 'command' parameter\"}"));
		MORPH_RETURN(-EINVAL);
	}
	if (tool_context_command_principal(command, principal,
					   sizeof(principal)) != 0)
		snprintf(principal, sizeof(principal), "%s", "shell");
	{
		enum tool_operation_verdict verdict;
		struct tool_operation op = {
			.kind = TOOL_OP_COMMAND,
			.tool_name = "bash_exec",
			.principal = principal,
			.action = command,
			.scope = cwd,
			.details_json = args_json,
		};

		rc = tool_context_check_operation_verdict(tctx, &op, &verdict);
		if (rc != 0) {
			cJSON_Delete(root);
			(void)tool_result_success_json_text(result, strdup(
				"{\"error\":\"command is not allowed by server policy\"}"));
			return rc;
		}
	}
	write_paths = cJSON_GetObjectItem(root, "write_paths");
	delete_paths = cJSON_GetObjectItem(root, "delete_paths");
	rc = prepare_policy_paths(tctx, principal, command, write_paths,
				  TOOL_PATH_WRITE, &approved_write, result);
	if (rc == 0)
		rc = prepare_policy_paths(tctx, principal, command, delete_paths,
					  TOOL_PATH_DELETE, &approved_delete,
					  result);
	if (rc != 0) {
		cJSON_Delete(approved_write);
		cJSON_Delete(approved_delete);
		cJSON_Delete(root);
		return rc;
	}
	effective_cwd = cwd;
	if (!effective_cwd || !*effective_cwd) {
		snprintf(workdir_buf, sizeof(workdir_buf), "%s",
			 tool_context_workdir(tctx));
		effective_cwd = workdir_buf;
	}
	if (!tctx->bash_exec_local_mode && effective_cwd && *effective_cwd &&
	    !path_in_configured_dirs(effective_cwd,
				     tctx->bash_exec_server_read_dirs,
				     tctx->bash_exec_server_read_dirs_count)) {
		cJSON_Delete(approved_write);
		cJSON_Delete(approved_delete);
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"cwd is outside server read_paths\"}"));
		MORPH_RETURN(-EPERM);
	}
	if (pipe(out_pipe) < 0) {
		rc = -errno;
		cJSON_Delete(approved_write);
		cJSON_Delete(approved_delete);
		cJSON_Delete(root);
		return rc;
	}
	if (pipe(err_pipe) < 0) {
		rc = -errno;
		close(out_pipe[0]);
		close(out_pipe[1]);
		cJSON_Delete(approved_write);
		cJSON_Delete(approved_delete);
		cJSON_Delete(root);
		return rc;
	}
	pid = fork();
	if (pid < 0) {
		rc = -errno;
		close(out_pipe[0]);
		close(out_pipe[1]);
		close(err_pipe[0]);
		close(err_pipe[1]);
		cJSON_Delete(approved_write);
		cJSON_Delete(approved_delete);
		cJSON_Delete(root);
		return rc;
	}
	if (pid == 0) {
		struct sandbox_config sb;
		const char **write_grants = NULL;
		const char **delete_grants = NULL;
		char *allowed_env[TOOL_CONTEXT_ALLOW_MAX];
		char **read_allowed;
		char **write_allowed;
		char **delete_allowed;
		char resolved_cwd[PATH_MAX];
		char resolved_tmp[PATH_MAX];
		int grant_capacity = (int)tctx->scoped_grants.nelts;
		int write_count = 0;
		int delete_count = 0;
		int read_count = 0;
		int allowed_env_count = 0;
		int write_grant_count = 0;
		int delete_grant_count = 0;
		int write_requested = approved_write ?
			cJSON_GetArraySize(approved_write) : 0;
		int delete_requested = approved_delete ?
			cJSON_GetArraySize(approved_delete) : 0;
		int have_tmp;
		int base = TOOL_CONTEXT_ALLOW_MAX + grant_capacity +
			write_requested + delete_requested + 4;

		close(out_pipe[0]);
		close(err_pipe[0]);
		(void)dup2(out_pipe[1], STDOUT_FILENO);
		(void)dup2(err_pipe[1], STDERR_FILENO);
		close(out_pipe[1]);
		close(err_pipe[1]);
		{
			int devnull = open("/dev/null", O_RDONLY);

			if (devnull >= 0) {
				(void)dup2(devnull, STDIN_FILENO);
				close(devnull);
			}
		}
		if (!effective_cwd || !realpath(effective_cwd, resolved_cwd) ||
		    chdir(resolved_cwd) != 0)
			_exit(126);
		have_tmp = realpath("/tmp", resolved_tmp) != NULL;
		read_allowed = calloc((size_t)base, sizeof(*read_allowed));
		write_allowed = calloc((size_t)base, sizeof(*write_allowed));
		delete_allowed = calloc((size_t)base, sizeof(*delete_allowed));
		if (!read_allowed || !write_allowed || !delete_allowed)
			_exit(126);
		if (grant_capacity > 0) {
			write_grants = calloc((size_t)grant_capacity,
					      sizeof(*write_grants));
			delete_grants = calloc((size_t)grant_capacity,
					       sizeof(*delete_grants));
			if (!write_grants || !delete_grants)
				_exit(126);
			write_grant_count = tool_context_collect_write_grants(
				tctx, principal, write_grants, grant_capacity);
			delete_grant_count = tool_context_collect_delete_grants(
				tctx, principal, delete_grants, grant_capacity);
		}
		memset(&sb, 0, sizeof(sb));
		sb.path_policy_enabled = 1;
		sb.process_exec = 1;
		sb.max_memory_mb = 512;
		sb.max_cpu_seconds = timeout;
		if (tctx->bash_exec_local_mode) {
			const char *workdir = tool_context_workdir(tctx);

			sb.read_all = 1;
			sb.network_access = 1;
			for (size_t i = 0;
			     i < sizeof(local_network_env) /
				 sizeof(local_network_env[0]); i++)
				allowed_env[allowed_env_count++] =
					(char *)local_network_env[i];
			if (workdir && *workdir) {
				add_allowed_path(write_allowed, &write_count,
						 workdir);
				add_allowed_path(delete_allowed, &delete_count,
						 workdir);
			}
			if (tool_context_output_dir(tctx)[0]) {
				add_allowed_path(write_allowed, &write_count,
					tool_context_output_dir(tctx));
				add_allowed_path(delete_allowed, &delete_count,
					tool_context_output_dir(tctx));
			}
			if (have_tmp) {
				add_allowed_path(write_allowed, &write_count,
					resolved_tmp);
				add_allowed_path(delete_allowed, &delete_count,
					resolved_tmp);
				if (strcmp(resolved_tmp, "/tmp") != 0) {
					add_allowed_path(write_allowed, &write_count,
						"/tmp");
					add_allowed_path(delete_allowed, &delete_count,
						"/tmp");
				}
			}
			add_json_paths(write_allowed, &write_count,
				       approved_write);
			add_json_paths(delete_allowed, &delete_count,
				       approved_delete);
			for (int i = 0; i < write_grant_count; i++)
				add_allowed_path(write_allowed, &write_count,
						 write_grants[i]);
			for (int i = 0; i < delete_grant_count; i++)
				add_allowed_path(delete_allowed, &delete_count,
						 delete_grants[i]);
		} else {
			sb.network_access =
				tctx->bash_exec_server_network_access;
			for (int i = 0;
			     i < tctx->bash_exec_server_allowed_env_count; i++)
				allowed_env[allowed_env_count++] =
					tctx->bash_exec_server_allowed_env[i];
			for (int i = 0;
			     i < tctx->bash_exec_server_read_dirs_count; i++) {
				add_allowed_path(read_allowed, &read_count,
					tctx->bash_exec_server_read_dirs[i]);
				if (have_tmp && strcmp(
				    tctx->bash_exec_server_read_dirs[i],
				    resolved_tmp) == 0)
					add_allowed_path(read_allowed, &read_count,
						"/tmp");
			}
			for (int i = 0;
			     i < tctx->bash_exec_server_write_dirs_count; i++) {
				add_allowed_path(write_allowed, &write_count,
					tctx->bash_exec_server_write_dirs[i]);
				if (have_tmp && strcmp(
				    tctx->bash_exec_server_write_dirs[i],
				    resolved_tmp) == 0)
					add_allowed_path(write_allowed, &write_count,
						"/tmp");
			}
			for (int i = 0;
			     i < tctx->bash_exec_server_delete_dirs_count; i++) {
				add_allowed_path(delete_allowed, &delete_count,
					tctx->bash_exec_server_delete_dirs[i]);
				if (have_tmp && strcmp(
				    tctx->bash_exec_server_delete_dirs[i],
				    resolved_tmp) == 0)
					add_allowed_path(delete_allowed, &delete_count,
						"/tmp");
			}
			if (read_count == 1 && strcmp(read_allowed[0], "/") == 0)
				sb.read_all = 1;
		}
		sb.read_paths = read_allowed;
		sb.read_paths_count = read_count;
		sb.write_paths = write_allowed;
		sb.write_paths_count = write_count;
		sb.delete_paths = delete_allowed;
		sb.delete_paths_count = delete_count;
		sb.allowed_env = allowed_env;
		sb.allowed_env_count = allowed_env_count;
		if (sandbox_enter(&sb) != 0)
			_exit(126);
		execl("/bin/sh", "sh", "-c", command, (char *)NULL);
		_exit(127);
	}
	close(out_pipe[1]);
	close(err_pipe[1]);
	{
		morph_buf_t out_buf;
		morph_buf_t err_buf;
		int timed_out = 0;
		int status = 0;
		int exit_code = -1;
		const char *signal_name = NULL;
		cJSON *out;
		char *text;

		rc = morph_buf_init(&out_buf, 4096);
		if (rc == 0)
			rc = morph_buf_init(&err_buf, 4096);
		if (rc != 0) {
			morph_buf_cleanup(&out_buf);
			close(out_pipe[0]);
			close(err_pipe[0]);
			(void)kill(pid, SIGKILL);
			(void)waitpid(pid, NULL, 0);
			cJSON_Delete(approved_write);
			cJSON_Delete(approved_delete);
			cJSON_Delete(root);
			return rc;
		}
		(void)read_pipes_with_timeout(out_pipe[0], err_pipe[0],
			&out_buf, &err_buf, timeout, &timed_out);
		close(out_pipe[0]);
		close(err_pipe[0]);
		if (timed_out)
			(void)kill(pid, SIGKILL);
		(void)waitpid(pid, &status, 0);
		if (WIFEXITED(status))
			exit_code = WEXITSTATUS(status);
		else if (WIFSIGNALED(status))
			signal_name = strsignal(WTERMSIG(status));
		out = cJSON_CreateObject();
		cJSON_AddStringToObject(out, "command", command);
		cJSON_AddStringToObject(out, "cwd", effective_cwd);
		cJSON_AddNumberToObject(out, "exit_code", exit_code);
		cJSON_AddBoolToObject(out, "timed_out", timed_out);
		cJSON_AddStringToObject(out, "stdout",
			out_buf.data ? out_buf.data : "");
		cJSON_AddStringToObject(out, "stderr",
			err_buf.data ? err_buf.data : "");
		cJSON_AddBoolToObject(out, "stdout_truncated",
			out_buf.len >= BASH_EXEC_MAX_OUTPUT);
		cJSON_AddBoolToObject(out, "stderr_truncated",
			err_buf.len >= BASH_EXEC_MAX_OUTPUT);
		if (signal_name)
			cJSON_AddStringToObject(out, "signal", signal_name);
		if (timed_out)
			cJSON_AddStringToObject(out, "error", "command timed out");
		else if (exit_code != 0)
			cJSON_AddStringToObject(out, "error",
						"command exited with non-zero status");
		text = cJSON_PrintUnformatted(out);
		cJSON_Delete(out);
		morph_buf_cleanup(&out_buf);
		morph_buf_cleanup(&err_buf);
		cJSON_Delete(approved_write);
		cJSON_Delete(approved_delete);
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result,
			text ? text : strdup("{}"));
	}
	return 0;
}

static int bash_exec_run(const char *args_json, struct tool_result *result,
			 void *user_data)
{
	struct tool_context *tctx = user_data;

	if (tctx && tctx->bash_exec_mode_configured)
		return bash_exec_run_policy(args_json, result, tctx);
	return bash_exec_run_legacy(args_json, result, user_data);
}

int bash_exec_init(struct tool_registry *reg, struct tool_context *tctx)
{
	if (!reg)
		return -EINVAL;
	int rc = tool_register(reg, &(struct tool_spec){ .origin = TOOL_ORIGIN_BUILTIN, .name = "bash_exec", .description = "Execute a shell command in a restricted sandboxed subprocess. "
		"Captures stdout/stderr and exit code. Use this to run "
		"commands described in skill instructions (build/test/lint/git/etc.). "
		"Local mode can read all files and use the network; writes and deletes "
		"are confined to workdir/output/tmp unless explicit path capabilities "
		"receive once/session approval. Server mode uses only configured read, "
		"write, delete, network, and command rules and never escalates. "
		"Args: command (required), cwd (optional working dir), "
		"write_paths (optional array of additional writable directories), "
		"delete_paths (optional array of additional removable directories), "
		"timeout_seconds (optional, default 60). "
		"Use 120-300 for builds, large test suites, or git operations; "
		"use 30 for quick queries.", .input_schema = "{\"type\":\"object\",\"properties\":{"
		"\"command\":{\"type\":\"string\",\"description\":\"shell command to execute via /bin/sh -c\"},"
		"\"cwd\":{\"type\":\"string\",\"description\":\"working directory\"},"
		"\"write_paths\":{\"type\":\"array\","
		"\"items\":{\"type\":\"string\"},"
		"\"description\":\"additional directory capabilities required "
		"for writes outside the default policy\"},"
		"\"delete_paths\":{\"type\":\"array\","
		"\"items\":{\"type\":\"string\"},"
		"\"description\":\"additional directory capabilities required "
		"for deletion or rename\"},"
		"\"timeout_seconds\":{\"type\":\"integer\",\"description\":\"max runtime in seconds (default 60; use 120-300 for builds/tests/git)\"}"
		"},\"required\":[\"command\"]}", .output_schema = TOOL_OBJECT_OUTPUT_SCHEMA, .exec = bash_exec_run, .user_data = tctx, .user_data_destroy = NULL });
	if (rc == 0) {
		struct tool_entry *e = tool_lookup(reg, "bash_exec");
		if (e)
			e->flags |= TOOL_FLAG_INTERNAL_APPROVAL;
	}
	return rc;
}
