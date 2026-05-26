#include "bash_exec.h"
#include "sandbox.h"
#include "util/log.h"
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
#define BASH_EXEC_ALLOW_MAX 32
#define BASH_EXEC_RULE_MAX 1024

static int bash_exec_default_timeout = 60;
static char bash_exec_allowed_commands[BASH_EXEC_ALLOW_MAX][BASH_EXEC_RULE_MAX];
static int bash_exec_allowed_commands_count;
static char bash_exec_allowed_cwds[BASH_EXEC_ALLOW_MAX][PATH_MAX];
static int bash_exec_allowed_cwds_count;
static bash_exec_approval_cb_t bash_exec_approval_cb;
static void *bash_exec_approval_user_data;

void bash_exec_set_default_timeout(int seconds)
{
	if (seconds > 0)
		bash_exec_default_timeout = seconds;
}

void bash_exec_clear_allowlist(void)
{
	memset(bash_exec_allowed_commands, 0, sizeof(bash_exec_allowed_commands));
	memset(bash_exec_allowed_cwds, 0, sizeof(bash_exec_allowed_cwds));
	bash_exec_allowed_commands_count = 0;
	bash_exec_allowed_cwds_count = 0;
	bash_exec_approval_cb = NULL;
	bash_exec_approval_user_data = NULL;
}

void bash_exec_set_approval_callback(bash_exec_approval_cb_t cb,
				     void *user_data)
{
	bash_exec_approval_cb = cb;
	bash_exec_approval_user_data = user_data;
}

int bash_exec_allow_command(const char *pattern)
{
	if (!pattern || !*pattern)
		MORPH_RETURN(-EINVAL);
	if (strlen(pattern) >= BASH_EXEC_RULE_MAX)
		MORPH_RETURN(-ENAMETOOLONG);
	for (int i = 0; i < bash_exec_allowed_commands_count; i++) {
		if (strcmp(bash_exec_allowed_commands[i], pattern) == 0)
			return 0;
	}
	if (bash_exec_allowed_commands_count >= BASH_EXEC_ALLOW_MAX)
		MORPH_RETURN(-ENOSPC);
	snprintf(bash_exec_allowed_commands[bash_exec_allowed_commands_count],
		 BASH_EXEC_RULE_MAX, "%s", pattern);
	bash_exec_allowed_commands_count++;
	return 0;
}

int bash_exec_allow_cwd(const char *path)
{
	char resolved[PATH_MAX];
	const char *stored;

	if (!path || !*path)
		MORPH_RETURN(-EINVAL);
	if (strcmp(path, "*") == 0) {
		stored = "*";
	} else {
		if (!realpath(path, resolved))
			MORPH_RETURN(-errno);
		stored = resolved;
	}
	for (int i = 0; i < bash_exec_allowed_cwds_count; i++) {
		if (strcmp(bash_exec_allowed_cwds[i], stored) == 0)
			return 0;
	}
	if (bash_exec_allowed_cwds_count >= BASH_EXEC_ALLOW_MAX)
		MORPH_RETURN(-ENOSPC);
	snprintf(bash_exec_allowed_cwds[bash_exec_allowed_cwds_count],
		 PATH_MAX, "%s", stored);
	bash_exec_allowed_cwds_count++;
	return 0;
}

static const char *skip_ws(const char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

static int has_whitespace(const char *s)
{
	for (; *s; s++) {
		if (*s == ' ' || *s == '\t')
			return 1;
	}
	return 0;
}

/*
 * Match a single command-allowlist pattern against a (whitespace-stripped)
 * command. Patterns:
 *   "*"              - wildcard, matches anything
 *   "prog" (no ws)   - first-token (program-name) match: command begins with
 *                      `prog` followed by EOS or whitespace
 *   "prefix*"        - prefix match: command begins with the literal prefix
 *                      and the prefix is followed by EOS or whitespace
 *   "exact args"     - exact string match
 */
static int command_matches_pattern(const char *cmd, const char *pat)
{
	cmd = skip_ws(cmd);
	pat = skip_ws(pat);
	if (!*pat)
		return 0;
	if (strcmp(pat, "*") == 0)
		return 1;
	size_t plen = strlen(pat);
	if (pat[plen - 1] == '*') {
		size_t base = plen - 1;
		while (base > 0 &&
		       (pat[base - 1] == ' ' || pat[base - 1] == '\t'))
			base--;
		if (base == 0)
			return 1;
		if (strncmp(cmd, pat, base) != 0)
			return 0;
		char after = cmd[base];
		return after == '\0' || after == ' ' || after == '\t';
	}
	if (!has_whitespace(pat)) {
		if (strncmp(cmd, pat, plen) != 0)
			return 0;
		char after = cmd[plen];
		return after == '\0' || after == ' ' || after == '\t';
	}
	return strcmp(cmd, pat) == 0;
}

static int command_is_allowed(const char *command)
{
	for (int i = 0; i < bash_exec_allowed_commands_count; i++) {
		if (command_matches_pattern(
			    command, bash_exec_allowed_commands[i]))
			return 1;
	}
	return 0;
}

static int cwd_matches_pattern(const char *resolved, const char *pat)
{
	if (strcmp(pat, "*") == 0)
		return 1;
	if (strcmp(resolved, pat) == 0)
		return 1;
	size_t plen = strlen(pat);
	if (plen == 0)
		return 0;
	if (strncmp(resolved, pat, plen) != 0)
		return 0;
	if (resolved[plen] != '/')
		return 0;
	/* "/" itself as root would match every absolute path; permit it. */
	return 1;
}

static int cwd_is_allowed(const char *cwd)
{
	char resolved[PATH_MAX];

	if (!cwd || !*cwd)
		return 1;
	if (!realpath(cwd, resolved))
		return 0;
	for (int i = 0; i < bash_exec_allowed_cwds_count; i++) {
		if (cwd_matches_pattern(
			    resolved, bash_exec_allowed_cwds[i]))
			return 1;
	}
	return 0;
}

/*
 * Extract a program name (first whitespace-delimited token, stripped of
 * any leading directory path) suitable for adding to the runtime command
 * allowlist. Returns 0 on success and writes a NUL-terminated string to
 * out; returns a negative error code otherwise.
 */
static int extract_program_name(const char *command, char *out, size_t outsize)
{
	if (!command || !out || outsize == 0)
		return -EINVAL;
	const char *p = skip_ws(command);
	const char *start = p;
	while (*p && *p != ' ' && *p != '\t' && *p != ';' && *p != '|' &&
	       *p != '&' && *p != '\n' && *p != '(' && *p != '`' &&
	       *p != '$')
		p++;
	if (p == start)
		return -EINVAL;
	const char *base = p;
	while (base > start && *(base - 1) != '/')
		base--;
	size_t len = (size_t)(p - base);
	if (len == 0 || len + 1 > outsize)
		return -EINVAL;
	memcpy(out, base, len);
	out[len] = '\0';
	return 0;
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
	"apt", "apt-get", "yum", "dnf", "brew", "pip", "pip3", "npm", "gem", "cargo",
	"curl", "wget",
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

struct buf {
	char *data;
	size_t len;
	size_t cap;
};

static int buf_append(struct buf *b, const char *s, size_t n)
{
	if (b->len >= BASH_EXEC_MAX_OUTPUT)
		return 0;
	if (b->len + n > BASH_EXEC_MAX_OUTPUT)
		n = BASH_EXEC_MAX_OUTPUT - b->len;
	if (b->len + n + 1 > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 4096;
		while (b->len + n + 1 > nc)
			nc *= 2;
		char *p = realloc(b->data, nc);
		if (!p)
			return -ENOMEM;
		b->data = p;
		b->cap = nc;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
	return 0;
}

static int read_pipes_with_timeout(int out_fd, int err_fd,
				   struct buf *out_buf, struct buf *err_buf,
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
			MORPH_RETURN(-errno);
		}
		if (rc == 0) {
			*timed_out = 1;
			return 0;
		}

		char tmp[4096];
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

static int bash_exec_run(const char *args_json, char **result_json,
			 void *user_data)
{
	(void)user_data;
	if (!result_json)
		return -EINVAL;

	cJSON *root = args_json ? cJSON_Parse(args_json) : NULL;
	const char *command = NULL;
	const char *cwd = NULL;
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
	}

	if (!command || !*command) {
		if (root)
			cJSON_Delete(root);
		*result_json = strdup(
			"{\"error\":\"missing 'command' parameter. "
			"Usage: bash_exec({\\\"command\\\": \\\"ls -la\\\"})\"}");
		return -EINVAL;
	}

	if (contains_blocked_command(command)) {
		log_warn("bash_exec: blocked dangerous command: %s", command);
		if (root)
			cJSON_Delete(root);
		*result_json = strdup(
			"{\"error\":\"command blocked for safety. "
			"Destructive operations (rm, mv, cp, chmod, curl, ssh, "
			"kill, package managers, etc.) are not allowed. "
			"Use read-only alternatives instead.\"}");
		return -EPERM;
	}

	int cmd_ok = command_is_allowed(command);
	int cwd_ok = cwd_is_allowed(cwd);
	if (!cmd_ok || !cwd_ok) {
		if (!bash_exec_approval_cb) {
			log_warn("bash_exec: command or cwd not allowed "
				 "and no approval callback registered");
			if (root)
				cJSON_Delete(root);
			*result_json = strdup(
				"{\"error\":\"command or cwd is not allowed "
				"by bash_exec policy and no interactive "
				"approval is available\"}");
			return -EPERM;
		}
		enum bash_exec_verdict v = bash_exec_approval_cb(
			command, cwd, bash_exec_approval_user_data);
		if (v == BASH_EXEC_DENY) {
			log_warn("bash_exec: command denied by user");
			if (root)
				cJSON_Delete(root);
			*result_json = strdup(
				"{\"error\":\"command execution denied "
				"by user\"}");
			return -EPERM;
		}
		if (v == BASH_EXEC_ALWAYS) {
			char prog[BASH_EXEC_RULE_MAX];
			if (!cmd_ok &&
			    extract_program_name(command, prog,
						 sizeof(prog)) == 0) {
				int rc = bash_exec_allow_command(prog);
				if (rc < 0)
					log_warn("bash_exec: failed to "
						 "persist program '%s' "
						 "(rc=%d)", prog, rc);
				else
					log_info("bash_exec: persisted "
						 "program '%s' for session",
						 prog);
			}
			if (!cwd_ok && cwd && *cwd) {
				int rc = bash_exec_allow_cwd(cwd);
				if (rc < 0)
					log_warn("bash_exec: failed to "
						 "persist cwd '%s' (rc=%d)",
						 cwd, rc);
				else
					log_info("bash_exec: persisted "
						 "cwd '%s' for session",
						 cwd);
			}
		} else {
			log_info("bash_exec: command approved once by user");
		}
	}

	int out_pipe[2], err_pipe[2];
	if (pipe(out_pipe) < 0) {
		if (root)
			cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"pipe() failed\"}");
		MORPH_RETURN(-errno);
	}
	if (pipe(err_pipe) < 0) {
		close(out_pipe[0]);
		close(out_pipe[1]);
		if (root)
			cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"pipe() failed\"}");
		MORPH_RETURN(-errno);
	}

	log_info("bash_exec: running '%s' (cwd=%s, timeout=%ds)",
		 command, cwd ? cwd : ".", timeout);

	pid_t pid = fork();
	if (pid < 0) {
		close(out_pipe[0]); close(out_pipe[1]);
		close(err_pipe[0]); close(err_pipe[1]);
		if (root)
			cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"fork() failed\"}");
		MORPH_RETURN(-errno);
	}

	if (pid == 0) {
		char resolved_cwd[PATH_MAX];
		const char *sandbox_cwd = cwd;

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

		if (cwd && *cwd) {
			if (!realpath(cwd, resolved_cwd) ||
			    chdir(resolved_cwd) != 0) {
				fprintf(stderr,
					"bash_exec: chdir(%s) failed: %s\n",
					cwd, strerror(errno));
				_exit(126);
			}
			sandbox_cwd = resolved_cwd;
		}

		struct sandbox_config sb;
		int sb_rc;
		memset(&sb, 0, sizeof(sb));
		sb.permissions = EXT_PERM_EXEC | EXT_PERM_NETWORK;
		if (cwd && *cwd)
			sb.permissions |= EXT_PERM_FILESYS;
		sb.max_memory_mb = 512;
		sb.max_cpu_seconds = timeout;
		if (sandbox_cwd && *sandbox_cwd)
			sb.allowed_paths = (char **)&sandbox_cwd;
		sb.allowed_paths_count =
			(sandbox_cwd && *sandbox_cwd) ? 1 : 0;
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

	struct buf out_buf = {0};
	struct buf err_buf = {0};
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

	char *str = cJSON_PrintUnformatted(out);
	cJSON_Delete(out);
	free(out_buf.data);
	free(err_buf.data);
	if (root)
		cJSON_Delete(root);

	*result_json = str ? str : strdup("{}");
	return 0;
}

int bash_exec_init(struct tool_registry *reg)
{
	if (!reg)
		return -EINVAL;
	return tool_register(reg, "bash_exec",
		"Execute a shell command in a restricted sandboxed subprocess. "
		"Captures stdout/stderr and exit code. Use this to run "
		"commands described in skill instructions (build/test/lint/git/etc.). "
		"Network and non-essential inherited environment variables are unavailable. "
		"File writes require an explicit cwd and are limited to that directory. "
		"Commands matching the configured allowlist run silently; "
		"anything else triggers an interactive approval prompt to the user "
		"(yes/no/always). DANGEROUS commands are blocked unconditionally: "
		"rm, mv, cp, chmod, curl, wget, ssh, kill, package managers, "
		"and other destructive operations. "
		"Args: command (required), cwd (optional working dir), "
		"timeout_seconds (optional, default 60). "
		"Use 120-300 for builds, large test suites, or git operations; "
		"use 30 for quick queries.",
		"{\"type\":\"object\",\"properties\":{"
		"\"command\":{\"type\":\"string\",\"description\":\"shell command to execute via /bin/sh -c\"},"
		"\"cwd\":{\"type\":\"string\",\"description\":\"working directory\"},"
		"\"timeout_seconds\":{\"type\":\"integer\",\"description\":\"max runtime in seconds (default 60; use 120-300 for builds/tests/git)\"}"
		"},\"required\":[\"command\"]}",
		bash_exec_run, NULL, NULL);
}
