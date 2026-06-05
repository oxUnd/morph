#include "tool_context.h"
#include "util/file.h"
#include "util/log.h"
#include "util/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

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

static void resolve_into(char *dst, size_t dst_size, const char *src)
{
	if (!src || !*src) {
		dst[0] = '\0';
		return;
	}
	char *resolved = file_resolve_path(src);
	if (resolved) {
		strncpy(dst, resolved, dst_size - 1);
		dst[dst_size - 1] = '\0';
		free(resolved);
	} else {
		char *expanded = file_expand_path(src);
		if (expanded) {
			strncpy(dst, expanded, dst_size - 1);
			dst[dst_size - 1] = '\0';
			free(expanded);
		} else {
			strncpy(dst, src, dst_size - 1);
			dst[dst_size - 1] = '\0';
		}
	}
}

static const char *path_op_name(enum tool_path_op op)
{
	switch (op) {
	case TOOL_PATH_READ:
		return "read";
	case TOOL_PATH_LIST:
		return "list";
	case TOOL_PATH_WRITE:
		return "write";
	}
	return "path";
}

static int path_op_is_read(enum tool_path_op op)
{
	return op == TOOL_PATH_READ || op == TOOL_PATH_LIST;
}

static int resolve_user_path(struct tool_context *tctx,
			     enum tool_path_op op,
			     const char *path,
			     char *resolved,
			     size_t resolved_size)
{
	char candidate[PATH_MAX];
	const char *base;
	char *expanded;
	char *rp;

	if (!tctx || !path || !resolved || resolved_size == 0)
		MORPH_RETURN(-EINVAL);

	expanded = file_expand_path(path);
	if (!expanded)
		MORPH_RETURN(-ENOMEM);

	if (expanded[0] == '/') {
		snprintf(candidate, sizeof(candidate), "%s", expanded);
	} else {
		base = path_op_is_read(op) ? tctx->workdir : tctx->output_dir;
		if (base && *base)
			snprintf(candidate, sizeof(candidate), "%s/%s",
				 base, expanded);
		else
			snprintf(candidate, sizeof(candidate), "%s", expanded);
	}
	free(expanded);

	rp = file_resolve_path(candidate);
	if (!rp)
		MORPH_RETURN(-ENOENT);
	snprintf(resolved, resolved_size, "%s", rp);
	free(rp);
	return 0;
}

static void add_allowed_dir(char dirs[][TOOL_CONTEXT_ALLOW_PATH_MAX],
			    int *count, const char *dir)
{
	char *resolved;
	const char *to_store;

	if (!dirs || !count || !dir)
		return;
	if (*count >= TOOL_CONTEXT_ALLOW_MAX)
		return;
	resolved = file_resolve_path(dir);
	to_store = resolved ? resolved : dir;
	for (int i = 0; i < *count; i++) {
		if (strcmp(dirs[i], to_store) == 0) {
			free(resolved);
			return;
		}
	}
	strncpy(dirs[*count], to_store, TOOL_CONTEXT_ALLOW_PATH_MAX - 1);
	dirs[*count][TOOL_CONTEXT_ALLOW_PATH_MAX - 1] = '\0';
	(*count)++;
	free(resolved);
}

static void add_parent_dir(char dirs[][TOOL_CONTEXT_ALLOW_PATH_MAX],
			   int *count, const char *path)
{
	char buf[PATH_MAX];
	char *slash;

	if (!path)
		return;
	snprintf(buf, sizeof(buf), "%s", path);
	slash = strrchr(buf, '/');
	if (!slash)
		return;
	if (slash == buf)
		*(slash + 1) = '\0';
	else
		*slash = '\0';
	add_allowed_dir(dirs, count, buf);
}

static int path_exists_for_read(enum tool_path_op op, const char *path)
{
	struct stat st;

	if (!path_op_is_read(op))
		return 1;
	if (stat(path, &st) != 0)
		return 0;
	if (op == TOOL_PATH_LIST && !S_ISDIR(st.st_mode))
		return 0;
	return 1;
}

struct tool_context *tool_context_create(const char *workdir,
					 const char *output_dir)
{
	struct tool_context *tctx = calloc(1, sizeof(*tctx));
	if (!tctx)
		return NULL;
	resolve_into(tctx->workdir, sizeof(tctx->workdir), workdir);
	resolve_into(tctx->output_dir, sizeof(tctx->output_dir), output_dir);
	tctx->path_approval_fn = NULL;
	tctx->path_approval_user_data = NULL;
	tctx->approval_fn = NULL;
	tctx->approval_user_data = NULL;
	tctx->read_allowed_dirs_count = 0;
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

const char *tool_context_workdir(const struct tool_context *tctx)
{
	if (!tctx)
		return NULL;
	return tctx->workdir;
}

const char *tool_context_output_dir(const struct tool_context *tctx)
{
	if (!tctx)
		return NULL;
	return tctx->output_dir;
}

void tool_context_add_allowed_dir(struct tool_context *tctx, const char *dir)
{
	if (!tctx)
		return;
	add_allowed_dir(tctx->allowed_dirs, &tctx->allowed_dirs_count, dir);
}

void tool_context_add_read_allowed_dir(struct tool_context *tctx,
				       const char *dir)
{
	if (!tctx)
		return;
	add_allowed_dir(tctx->read_allowed_dirs,
			&tctx->read_allowed_dirs_count, dir);
}

void tool_context_set_path_approval(struct tool_context *tctx,
				    tool_path_approval_fn fn,
				    void *user_data)
{
	if (!tctx)
		return;
	tctx->path_approval_fn = fn;
	tctx->path_approval_user_data = user_data;
}

int tool_context_authorize_path(struct tool_context *tctx,
				enum tool_path_op op, const char *path,
				char *resolved, size_t resolved_size)
{
	char path_buf[PATH_MAX];
	const char *root;
	char (*allow_dirs)[TOOL_CONTEXT_ALLOW_PATH_MAX];
	int *allow_count;
	int rc;

	if (!tctx || !path)
		MORPH_RETURN(-EINVAL);
	if (!resolved || resolved_size == 0) {
		resolved = path_buf;
		resolved_size = sizeof(path_buf);
	}

	rc = resolve_user_path(tctx, op, path, resolved, resolved_size);
	if (rc < 0)
		return rc;

	if (path_op_is_read(op) && !path_exists_for_read(op, resolved))
		MORPH_RETURN(-ENOENT);

	root = path_op_is_read(op) ? tctx->workdir : tctx->output_dir;
	allow_dirs = path_op_is_read(op) ? tctx->read_allowed_dirs
					  : tctx->allowed_dirs;
	allow_count = path_op_is_read(op) ? &tctx->read_allowed_dirs_count
					   : &tctx->allowed_dirs_count;

	if (root && *root && path_is_within(resolved, root))
		return 0;
	for (int i = 0; i < *allow_count; i++) {
		if (path_is_within(resolved, allow_dirs[i]))
			return 0;
	}

	if (tctx->path_approval_fn) {
		enum tool_path_verdict v = tctx->path_approval_fn(
			op, resolved, root, tctx->path_approval_user_data);
		if (v == TOOL_PATH_ALLOW)
			return 0;
		if (v == TOOL_PATH_ALWAYS) {
			add_parent_dir(allow_dirs, allow_count, resolved);
			return 0;
		}
	} else if (op == TOOL_PATH_WRITE && tctx->approval_fn) {
		enum write_verdict v = tctx->approval_fn(
			resolved, tctx->output_dir, tctx->approval_user_data);
		if (v == WRITE_ALLOW)
			return 0;
		if (v == WRITE_ALWAYS) {
			add_parent_dir(tctx->allowed_dirs,
				       &tctx->allowed_dirs_count,
				       resolved);
			return 0;
		}
	}

	log_warn("%s path denied (outside root): %s",
		 path_op_name(op), resolved);
	MORPH_RETURN(-EPERM);
}

int tool_context_check_read_path(struct tool_context *tctx, const char *path)
{
	return tool_context_authorize_path(tctx, TOOL_PATH_READ, path,
					   NULL, 0);
}

int tool_context_check_write_path(struct tool_context *tctx, const char *path)
{
	return tool_context_authorize_path(tctx, TOOL_PATH_WRITE, path,
					   NULL, 0);
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
