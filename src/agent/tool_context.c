#include "tool_context.h"
#include "util/file.h"
#include "util/log.h"
#include "util/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>

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
	return 1;
}

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

struct tool_context *tool_context_create(const char *output_dir)
{
	struct tool_context *tctx = calloc(1, sizeof(*tctx));
	if (!tctx)
		return NULL;
	char *resolved = file_resolve_path(output_dir);
	if (resolved) {
		strncpy(tctx->output_dir, resolved,
			sizeof(tctx->output_dir) - 1);
		free(resolved);
	} else {
		strncpy(tctx->output_dir, output_dir,
			sizeof(tctx->output_dir) - 1);
	}
	tctx->approval_fn = NULL;
	tctx->approval_user_data = NULL;
	tctx->allowed_dirs_count = 0;
	tctx->command_approval_fn = NULL;
	tctx->command_approval_user_data = NULL;
	tctx->allowed_commands_count = 0;
	tctx->exec_allowed_dirs_count = 0;
	return tctx;
}

void tool_context_destroy(struct tool_context *tctx)
{
	free(tctx);
}

const char *tool_context_output_dir(const struct tool_context *tctx)
{
	if (!tctx)
		return NULL;
	return tctx->output_dir;
}

void tool_context_add_allowed_dir(struct tool_context *tctx, const char *dir)
{
	if (!tctx || !dir)
		return;
	if (tctx->allowed_dirs_count >= TOOL_CONTEXT_ALLOW_MAX)
		return;
	char *resolved = file_resolve_path(dir);
	const char *to_store = resolved ? resolved : dir;
	for (int i = 0; i < tctx->allowed_dirs_count; i++) {
		if (strcmp(tctx->allowed_dirs[i], to_store) == 0) {
			free(resolved);
			return;
		}
	}
	strncpy(tctx->allowed_dirs[tctx->allowed_dirs_count],
		to_store, TOOL_CONTEXT_ALLOW_PATH_MAX - 1);
	tctx->allowed_dirs_count++;
	free(resolved);
}

int tool_context_check_write_path(struct tool_context *tctx, const char *path)
{
	if (!tctx || !path)
		MORPH_RETURN(-EINVAL);
	if (path_is_within(path, tctx->output_dir))
		return 0;
	for (int i = 0; i < tctx->allowed_dirs_count; i++) {
		if (path_is_within(path, tctx->allowed_dirs[i]))
			return 0;
	}
	if (tctx->approval_fn) {
		enum write_verdict v = tctx->approval_fn(
			path, tctx->output_dir, tctx->approval_user_data);
		if (v == WRITE_ALLOW)
			return 0;
		if (v == WRITE_ALWAYS) {
			char *resolved = file_resolve_path(path);
			if (resolved) {
				char *slash = strrchr(resolved, '/');
				if (slash && slash != resolved) {
					*slash = '\0';
					tool_context_add_allowed_dir(tctx, resolved);
				}
			}
			free(resolved);
			return 0;
		}
	}
	log_warn("write path denied (outside output_dir): %s", path);
	MORPH_RETURN(-EPERM);
}

void tool_context_set_command_approval(struct tool_context *tctx,
				       tool_command_approval_fn fn,
				       void *user_data)
{
	if (!tctx)
		return;
	tctx->command_approval_fn = fn;
	tctx->command_approval_user_data = user_data;
}

int tool_context_allow_command(struct tool_context *tctx, const char *pattern)
{
	if (!tctx || !pattern || !*pattern)
		MORPH_RETURN(-EINVAL);
	if (strlen(pattern) >= TOOL_CONTEXT_COMMAND_MAX)
		MORPH_RETURN(-ENAMETOOLONG);
	for (int i = 0; i < tctx->allowed_commands_count; i++) {
		if (strcmp(tctx->allowed_commands[i], pattern) == 0)
			return 0;
	}
	if (tctx->allowed_commands_count >= TOOL_CONTEXT_ALLOW_MAX)
		MORPH_RETURN(-ENOSPC);
	snprintf(tctx->allowed_commands[tctx->allowed_commands_count],
		 TOOL_CONTEXT_COMMAND_MAX, "%s", pattern);
	tctx->allowed_commands_count++;
	return 0;
}

int tool_context_allow_exec_dir(struct tool_context *tctx, const char *path)
{
	char resolved[PATH_MAX];
	const char *stored;

	if (!tctx || !path || !*path)
		MORPH_RETURN(-EINVAL);
	if (strcmp(path, "*") == 0) {
		stored = "*";
	} else {
		if (!realpath(path, resolved))
			MORPH_RETURN(-errno);
		stored = resolved;
	}
	for (int i = 0; i < tctx->exec_allowed_dirs_count; i++) {
		if (strcmp(tctx->exec_allowed_dirs[i], stored) == 0)
			return 0;
	}
	if (tctx->exec_allowed_dirs_count >= TOOL_CONTEXT_ALLOW_MAX)
		MORPH_RETURN(-ENOSPC);
	snprintf(tctx->exec_allowed_dirs[tctx->exec_allowed_dirs_count],
		 TOOL_CONTEXT_ALLOW_PATH_MAX, "%s", stored);
	tctx->exec_allowed_dirs_count++;
	return 0;
}

int tool_context_check_command(struct tool_context *tctx,
			       const char *command, const char *cwd)
{
	if (!tctx || !command)
		MORPH_RETURN(-EINVAL);

	int cmd_ok = 0;
	for (int i = 0; i < tctx->allowed_commands_count; i++) {
		if (command_matches_pattern(command,
					    tctx->allowed_commands[i])) {
			cmd_ok = 1;
			break;
		}
	}

	int cwd_ok = 1;
	if (cwd && *cwd) {
		char resolved[PATH_MAX];
		cwd_ok = 0;
		if (realpath(cwd, resolved)) {
			for (int i = 0; i < tctx->exec_allowed_dirs_count; i++) {
				if (cwd_matches_pattern(
					    resolved,
					    tctx->exec_allowed_dirs[i])) {
					cwd_ok = 1;
					break;
				}
			}
		}
	}

	if (cmd_ok && cwd_ok)
		return 0;

	if (!tctx->command_approval_fn) {
		log_warn("command not allowed and no approval callback: %s",
			 command);
		MORPH_RETURN(-EPERM);
	}

	enum command_verdict v = tctx->command_approval_fn(
		command, cwd, tctx->command_approval_user_data);
	if (v == COMMAND_DENY) {
		log_warn("command denied by user: %s", command);
		MORPH_RETURN(-EACCES);
	}
	if (v == COMMAND_ALWAYS) {
		if (!cmd_ok) {
			char prog[TOOL_CONTEXT_COMMAND_MAX];
			if (extract_program_name(command, prog,
						 sizeof(prog)) == 0) {
				int rc = tool_context_allow_command(tctx, prog);
				if (rc < 0)
					log_warn("failed to persist command "
						 "'%s' (rc=%d)", prog, rc);
				else
					log_info("persisted command '%s' "
						 "for session", prog);
			}
		}
		if (!cwd_ok && cwd && *cwd) {
			int rc = tool_context_allow_exec_dir(tctx, cwd);
			if (rc < 0)
				log_warn("failed to persist cwd '%s' "
					 "(rc=%d)", cwd, rc);
			else
				log_info("persisted cwd '%s' for session",
					 cwd);
		}
	} else {
		log_info("command approved once by user");
	}
	return 0;
}
