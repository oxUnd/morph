#include "bash_exec.h"
#include "agent/tool_context.h"
#include "sandbox.h"
#include "util/log.h"
#include "util/buf.h"
#include "util/error.h"
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

void bash_exec_set_default_timeout(int seconds)
{
	if (seconds > 0)
		bash_exec_default_timeout = seconds;
}

static const char *blocked_commands[] = {
	"rm", "rmdir",
	"mkfs", "dd",
	"shutdown", "reboot", "poweroff", "halt",
	"init",
	"mv", "cp",
	"chmod", "chown", "chgrp", "chattr",
	"useradd", "userdel", "usermod",
	"groupadd", "groupdel",
	"passwd",
	"crontab",
	"systemctl", "service",
	"launchctl",
	"kill", "killall", "pkill",
	"ssh", "scp", "sftp", "rsync",
	"mount", "umount",
	"fdisk", "parted", "diskutil",
	"sysctl",
	"iptables", "ip6tables",
	"defaults",
	NULL
};

static int is_blocked_command(const char *cmd)
{
	while (*cmd == ' ' || *cmd == '\t')
		cmd++;
	const char *end = cmd;
	while (*end && *end != ' ' && *end != '\t' && *end != ';' &&
	       *end != '&' && *end != '|' && *end != '\n' && *end != '(' &&
	       *end != '`' && *end != '$')
		end++;
	size_t len = (size_t)(end - cmd);
	if (len == 0)
		return 0;
	const char *base = end;
	while (base > cmd && *(base - 1) != '/')
		base--;
	size_t base_len = (size_t)(end - base);
	for (const char **p = blocked_commands; *p; p++) {
		size_t blen = strlen(*p);
		if (base_len == blen && strncmp(base, *p, blen) == 0)
			return 1;
		if (base_len > blen && base[blen] == '.' &&
		    strncmp(base, *p, blen) == 0)
			return 1;
		if (len == blen && strncmp(cmd, *p, blen) == 0)
			return 1;
	}
	return 0;
}

static int contains_blocked_command(const char *cmd)
{
	if (is_blocked_command(cmd))
		return 1;
	const char *p = cmd;
	while (*p) {
		if (*p == ';' || *p == '&' || *p == '|' || *p == '`' ||
		    *p == '\n' || *p == '(') {
			p++;
			while (*p == ' ' || *p == '\t' || *p == '&' || *p == '|')
				p++;
			if (is_blocked_command(p))
				return 1;
		} else {
			p++;
		}
	}
	return 0;
}

static int command_has_compound_syntax(const char *command)
{
	return command && strpbrk(command, ";|&\n`$<>()") != NULL;
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

static int bash_exec_run(const char *args_json, struct tool_result *result,
			 void *user_data)
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

	if (contains_blocked_command(command)) {
		log_warn("bash_exec: blocked dangerous command: %s", command);
		if (root)
			cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"command blocked for safety. "
			"Destructive operations (rm, mv, cp, chmod, ssh, "
			"kill, etc.) are not allowed. "
			"Use read-only alternatives instead.\"}"));
		return -EPERM;
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
		struct tool_operation op = {
			.kind = TOOL_OP_COMMAND,
			.tool_name = "bash_exec",
			.principal = principal,
			.action = command,
			.target = NULL,
			.scope = cwd,
			.details_json = args_json,
		};
		int rc = tool_context_check_operation(tctx, &op);
		if (rc < 0) {
			if (root)
				cJSON_Delete(root);
			if (rc == -EACCES)
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

				if (!cJSON_IsString(path) || !path->valuestring) {
					cJSON_Delete(root);
					(void)tool_result_success_json_text(
						result, strdup(
						"{\"error\":\"write_paths must "
						"contain only strings\"}"));
					return -EINVAL;
				}
				path_rc = tool_context_request_write_access(
					tctx, principal, command,
					path->valuestring, resolved,
					sizeof(resolved));
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
		int requested_count = approved_write_paths ?
			cJSON_GetArraySize(approved_write_paths) : 0;
		int grant_capacity = tctx ?
			(int)tctx->scoped_grants.nelts : 0;
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
		allowed = calloc((size_t)(2 + requested_count +
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
			allowed[allowed_count++] = (char *)sandbox_cwd;
		}
		if (tctx) {
			const char *od = tool_context_output_dir(tctx);
			if (od && *od) {
				char resolved_od[PATH_MAX];
				if (realpath(od, resolved_od)) {
					if (!sandbox_cwd ||
					    strcmp(resolved_od, sandbox_cwd) != 0)
						allowed[allowed_count++] = resolved_od;
				}
			}
		}
		for (int i = 0; i < requested_count; i++) {
			cJSON *path = cJSON_GetArrayItem(
				approved_write_paths, i);
			if (cJSON_IsString(path) && path->valuestring)
				allowed[allowed_count++] = path->valuestring;
		}
		for (int i = 0; i < grant_count; i++)
			allowed[allowed_count++] = (char *)grants[i];
		if (requested_count > 0 || grant_count > 0)
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

int bash_exec_init(struct tool_registry *reg, struct tool_context *tctx)
{
	if (!reg)
		return -EINVAL;
	int rc = tool_register(reg, &(struct tool_spec){ .origin = TOOL_ORIGIN_BUILTIN, .name = "bash_exec", .description = "Execute a shell command in a restricted sandboxed subprocess. "
		"Captures stdout/stderr and exit code. Use this to run "
		"commands described in skill instructions (build/test/lint/git/etc.). "
		"Network and non-essential inherited environment variables are unavailable. "
		"File writes are limited to cwd/output unless write_paths requests "
		"additional directory capabilities. Approved session or persistent "
		"grants are reused automatically. "
		"Commands matching the configured allowlist run silently; "
		"anything else triggers an interactive approval prompt to the user "
		"(yes/no/always). DANGEROUS commands are blocked unconditionally: "
		"rm, mv, cp, chmod, ssh, kill, and other destructive operations. "
		"Network/download commands and package manager commands require "
		"approval unless explicitly allowlisted. "
		"Args: command (required), cwd (optional working dir), "
		"write_paths (optional array of additional writable directories), "
		"timeout_seconds (optional, default 60). "
		"Use 120-300 for builds, large test suites, or git operations; "
		"use 30 for quick queries.", .input_schema = "{\"type\":\"object\",\"properties\":{"
		"\"command\":{\"type\":\"string\",\"description\":\"shell command to execute via /bin/sh -c\"},"
		"\"cwd\":{\"type\":\"string\",\"description\":\"working directory\"},"
		"\"write_paths\":{\"type\":\"array\","
		"\"items\":{\"type\":\"string\"},"
		"\"description\":\"additional directory capabilities required "
		"for writes outside cwd/output\"},"
		"\"timeout_seconds\":{\"type\":\"integer\",\"description\":\"max runtime in seconds (default 60; use 120-300 for builds/tests/git)\"}"
		"},\"required\":[\"command\"]}", .output_schema = TOOL_OBJECT_OUTPUT_SCHEMA, .exec = bash_exec_run, .user_data = tctx, .user_data_destroy = NULL });
	if (rc == 0) {
		struct tool_entry *e = tool_lookup(reg, "bash_exec");
		if (e)
			e->flags |= TOOL_FLAG_INTERNAL_APPROVAL;
	}
	return rc;
}
