#include "tool_context.h"
#include "db/permission_grant.h"
#include "util/file.h"
#include "util/log.h"
#include "util/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#define TOOL_GRANT_SUBJECT_MAX 256

struct tool_scoped_grant {
	char subject[TOOL_GRANT_SUBJECT_MAX];
	char path[PATH_MAX];
};

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

int tool_context_command_principal(const char *command, char *out,
				   size_t out_size)
{
	char program[PATH_MAX];
	const char *path;
	const char *start;
	const char *end;

	if (!command || !out || out_size == 0)
		MORPH_RETURN(-EINVAL);
	start = skip_ws(command);
	end = start;
	while (*end && *end != ' ' && *end != '\t' && *end != ';' &&
	       *end != '|' && *end != '&' && *end != '\n')
		end++;
	if (end == start || (size_t)(end - start) >= sizeof(program))
		MORPH_RETURN(-EINVAL);
	memcpy(program, start, (size_t)(end - start));
	program[end - start] = '\0';
	if (strchr(program, '/')) {
		char *resolved = file_resolve_path(program);
		if (!resolved)
			MORPH_RETURN(-ENOENT);
		snprintf(out, out_size, "%s", resolved);
		free(resolved);
		return 0;
	}
	path = getenv("PATH");
	if (path && *path) {
		char *copy = strdup(path);
		char *save = NULL;
		char *dir;

		if (!copy)
			MORPH_RETURN(-ENOMEM);
		for (dir = strtok_r(copy, ":", &save); dir;
		     dir = strtok_r(NULL, ":", &save)) {
			char candidate[PATH_MAX];
			char *resolved;

			if (file_path_join(candidate, sizeof(candidate), dir,
					   program) != 0 ||
			    access(candidate, X_OK) != 0)
				continue;
			resolved = file_resolve_path(candidate);
			if (resolved) {
				snprintf(out, out_size, "%s", resolved);
				free(resolved);
				free(copy);
				return 0;
			}
		}
		free(copy);
	}
	snprintf(out, out_size, "%s", program);
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

static const char *operation_kind_name(enum tool_operation_kind kind)
{
	switch (kind) {
	case TOOL_OP_COMMAND:
		return "command";
	case TOOL_OP_PATH_READ:
		return "read";
	case TOOL_OP_PATH_LIST:
		return "list";
	case TOOL_OP_PATH_WRITE:
		return "write";
	case TOOL_OP_NETWORK:
		return "network";
	case TOOL_OP_EXTERNAL_SEND:
		return "external_send";
	}
	return "operation";
}

static int path_op_is_read(enum tool_path_op op)
{
	return op == TOOL_PATH_READ || op == TOOL_PATH_LIST;
}

static enum tool_operation_kind path_op_to_kind(enum tool_path_op op)
{
	switch (op) {
	case TOOL_PATH_READ:
		return TOOL_OP_PATH_READ;
	case TOOL_PATH_LIST:
		return TOOL_OP_PATH_LIST;
	case TOOL_PATH_WRITE:
		return TOOL_OP_PATH_WRITE;
	}
	return TOOL_OP_PATH_READ;
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

	base = path_op_is_read(op) ? tctx->workdir : tctx->output_dir;
	if ((!base || !*base) && !file_path_is_absolute(expanded))
		base = "";
	if (file_path_full(candidate, sizeof(candidate), base, expanded) != 0) {
		free(expanded);
		MORPH_RETURN(-ENAMETOOLONG);
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

static int add_scoped_write_grant(struct tool_context *tctx,
				  const char *subject, const char *path)
{
	struct tool_scoped_grant grant;
	struct tool_scoped_grant *existing;

	if (!tctx || !subject || !*subject || !path || !*path)
		MORPH_RETURN(-EINVAL);
	morph_array_foreach(existing, &tctx->scoped_grants,
			    struct tool_scoped_grant) {
		if (strcmp(existing->subject, subject) == 0 &&
		    strcmp(existing->path, path) == 0)
			return 0;
	}
	memset(&grant, 0, sizeof(grant));
	snprintf(grant.subject, sizeof(grant.subject), "%s", subject);
	snprintf(grant.path, sizeof(grant.path), "%s", path);
	existing = morph_array_push(&tctx->scoped_grants);
	if (!existing)
		MORPH_RETURN(-ENOMEM);
	*existing = grant;
	return 0;
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
	tctx->operation_approval_fn = NULL;
	tctx->operation_approval_user_data = NULL;
	tctx->read_allowed_dirs_count = 0;
	tctx->write_allowed_dirs_count = 0;
	if (tctx->output_dir[0] &&
	    !(tctx->workdir[0] && path_is_within(tctx->output_dir,
						 tctx->workdir))) {
		add_allowed_dir(tctx->read_allowed_dirs,
				&tctx->read_allowed_dirs_count,
				tctx->output_dir);
	}
	tctx->allowed_commands_count = 0;
	tctx->exec_allowed_dirs_count = 0;
	if (morph_array_init(&tctx->scoped_grants, 8,
			     sizeof(struct tool_scoped_grant)) != 0) {
		free(tctx);
		return NULL;
	}
	tctx->grant_db = NULL;
	tctx->grant_project_root[0] = '\0';
	tctx->default_timeout_seconds = 0;
	return tctx;
}

void tool_context_destroy(struct tool_context *tctx)
{
	if (tctx)
		morph_array_cleanup(&tctx->scoped_grants);
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

void tool_context_set_default_timeout(struct tool_context *tctx, int seconds)
{
	if (!tctx)
		return;
	tctx->default_timeout_seconds = seconds > 0 ? seconds : 0;
}

int tool_context_default_timeout(const struct tool_context *tctx)
{
	if (!tctx)
		return 0;
	return tctx->default_timeout_seconds;
}

void tool_context_add_read_allowed_dir(struct tool_context *tctx,
				       const char *dir)
{
	if (!tctx)
		return;
	add_allowed_dir(tctx->read_allowed_dirs,
			&tctx->read_allowed_dirs_count, dir);
}

void tool_context_add_write_allowed_dir(struct tool_context *tctx,
					const char *dir)
{
	if (!tctx)
		return;
	add_allowed_dir(tctx->write_allowed_dirs,
			&tctx->write_allowed_dirs_count, dir);
}

void tool_context_set_operation_approval(struct tool_context *tctx,
					 tool_operation_approval_fn fn,
					 void *user_data)
{
	if (!tctx)
		return;
	tctx->operation_approval_fn = fn;
	tctx->operation_approval_user_data = user_data;
}

static int load_grant(const struct permission_grant *grant, void *user_data)
{
	struct tool_context *tctx = user_data;

	if (strcmp(grant->resource_kind, "command") == 0)
		return tool_context_allow_command_pattern(tctx, grant->resource);
	if (strcmp(grant->resource_kind, "command_cwd") == 0)
		return tool_context_allow_command_scope(tctx, grant->resource);
	if (strcmp(grant->resource_kind, "write_path") == 0)
		return add_scoped_write_grant(tctx, grant->subject,
					      grant->resource);
	if (strcmp(grant->resource_kind, "read_path") == 0) {
		add_allowed_dir(tctx->read_allowed_dirs,
				&tctx->read_allowed_dirs_count,
				grant->resource);
		return 0;
	}
	if (strcmp(grant->resource_kind, "tool_write_path") == 0) {
		add_allowed_dir(tctx->write_allowed_dirs,
				&tctx->write_allowed_dirs_count,
				grant->resource);
		return 0;
	}
	return 0;
}

int tool_context_set_grant_store(struct tool_context *tctx, struct db *db,
				 const char *project_root)
{
	char *resolved;
	int rc;

	if (!tctx || !db || !project_root || !*project_root)
		MORPH_RETURN(-EINVAL);
	resolved = file_resolve_path(project_root);
	if (!resolved)
		MORPH_RETURN(-ENOMEM);
	snprintf(tctx->grant_project_root, sizeof(tctx->grant_project_root),
		 "%s", resolved);
	free(resolved);
	tctx->grant_db = db;
	rc = permission_grant_each(db, tctx->grant_project_root,
				   load_grant, tctx);
	if (rc != 0) {
		tctx->grant_db = NULL;
		tctx->grant_project_root[0] = '\0';
	}
	return rc;
}

static int save_grant(struct tool_context *tctx, const char *subject,
		      const char *kind, const char *resource)
{
	struct permission_grant grant;

	if (!tctx || !tctx->grant_db || !tctx->grant_project_root[0])
		return 0;
	memset(&grant, 0, sizeof(grant));
	snprintf(grant.subject, sizeof(grant.subject), "%s",
		 subject ? subject : "*");
	snprintf(grant.resource_kind, sizeof(grant.resource_kind), "%s", kind);
	snprintf(grant.resource, sizeof(grant.resource), "%s", resource);
	snprintf(grant.project_root, sizeof(grant.project_root), "%s",
		 tctx->grant_project_root);
	return permission_grant_save(tctx->grant_db, &grant);
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
	struct tool_operation operation;

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
					  : tctx->write_allowed_dirs;
	allow_count = path_op_is_read(op) ? &tctx->read_allowed_dirs_count
					   : &tctx->write_allowed_dirs_count;

	if (root && *root && path_is_within(resolved, root))
		return 0;
	for (int i = 0; i < *allow_count; i++) {
		if (path_is_within(resolved, allow_dirs[i]))
			return 0;
	}

	memset(&operation, 0, sizeof(operation));
	operation.kind = path_op_to_kind(op);
	operation.tool_name = "tool";
	operation.principal = "tool:*";
	operation.target = resolved;
	operation.scope = root;

	rc = tool_context_check_operation(tctx, &operation);
	if (rc == 0)
		return 0;

	log_warn("%s path denied (outside root): %s",
		 operation_kind_name(path_op_to_kind(op)), resolved);
	return rc;
}

int tool_context_allow_command_pattern(struct tool_context *tctx,
				       const char *pattern)
{
	if (!tctx || !pattern || !*pattern)
		MORPH_RETURN(-EINVAL);
	if (strlen(pattern) >= TOOL_CONTEXT_ACTION_MAX)
		MORPH_RETURN(-ENAMETOOLONG);
	for (int i = 0; i < tctx->allowed_commands_count; i++) {
		if (strcmp(tctx->allowed_commands[i], pattern) == 0)
			return 0;
	}
	if (tctx->allowed_commands_count >= TOOL_CONTEXT_ALLOW_MAX)
		MORPH_RETURN(-ENOSPC);
	snprintf(tctx->allowed_commands[tctx->allowed_commands_count],
		 TOOL_CONTEXT_ACTION_MAX, "%s", pattern);
	tctx->allowed_commands_count++;
	return 0;
}

int tool_context_allow_command_scope(struct tool_context *tctx, const char *path)
{
	char resolved[PATH_MAX];
	const char *stored;

	if (!tctx || !path || !*path)
		MORPH_RETURN(-EINVAL);
	if (strcmp(path, "*") == 0) {
		stored = "*";
	} else {
		if (!realpath(path, resolved))
			MORPH_RETURN_ERRNO();
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

static int command_is_allowed(struct tool_context *tctx, const char *command)
{
	for (int i = 0; i < tctx->allowed_commands_count; i++) {
		if (command_matches_pattern(command,
					    tctx->allowed_commands[i]))
			return 1;
	}
	return 0;
}

static int command_scope_is_allowed(struct tool_context *tctx, const char *cwd)
{
	if (cwd && *cwd) {
		char resolved[PATH_MAX];
		if (realpath(cwd, resolved)) {
			for (int i = 0; i < tctx->exec_allowed_dirs_count; i++) {
				if (cwd_matches_pattern(
					    resolved,
					    tctx->exec_allowed_dirs[i]))
					return 1;
			}
		}
		return 0;
	}
	return 1;
}

static int check_command_operation(struct tool_context *tctx,
				   const struct tool_operation *op)
{
	const char *command = op->action;
	const char *cwd = op->scope;
	int cmd_ok;
	int cwd_ok;
	enum tool_operation_verdict v;

	if (!command)
		MORPH_RETURN(-EINVAL);

	cmd_ok = command_is_allowed(tctx, command);
	cwd_ok = command_scope_is_allowed(tctx, cwd);
	if (cmd_ok && cwd_ok)
		return 0;
	if (!tctx->operation_approval_fn) {
		log_warn("command not allowed and no approval callback: %s",
			 command);
		MORPH_RETURN(-EPERM);
	}
	v = tctx->operation_approval_fn(op,
					tctx->operation_approval_user_data);
	if (v == TOOL_OP_DENY) {
		log_warn("command denied by user: %s", command);
		MORPH_RETURN(-EACCES);
	}
	if (v == TOOL_OP_SESSION || v == TOOL_OP_ALWAYS) {
		if (!cmd_ok) {
			char prog[TOOL_CONTEXT_ACTION_MAX];
			if (extract_program_name(command, prog,
						 sizeof(prog)) == 0) {
				int rc = tool_context_allow_command_pattern(
					tctx, prog);
				if (rc < 0)
					log_warn("failed to persist command "
						 "'%s' (rc=%d)", prog, rc);
				else
					log_info("persisted command '%s' "
						 "for session", prog);
				if (v == TOOL_OP_ALWAYS && rc == 0)
					(void)save_grant(tctx, prog, "command",
							 prog);
			}
		}
		if (!cwd_ok && cwd && *cwd) {
			int rc = tool_context_allow_command_scope(tctx, cwd);
			if (rc < 0)
				log_warn("failed to persist cwd '%s' "
					 "(rc=%d)", cwd, rc);
			else
				log_info("persisted cwd '%s' for session",
					 cwd);
			if (v == TOOL_OP_ALWAYS && rc == 0)
				(void)save_grant(tctx,
						 op->principal ?
						 op->principal : "*",
						 "command_cwd", cwd);
		}
	} else {
		log_info("command approved once by user");
	}
	return 0;
}

static int check_path_operation(struct tool_context *tctx,
				const struct tool_operation *op)
{
	char (*allow_dirs)[TOOL_CONTEXT_ALLOW_PATH_MAX];
	int *allow_count;
	enum tool_operation_verdict v;

	if (!op->target)
		MORPH_RETURN(-EINVAL);
	if (op->kind == TOOL_OP_PATH_WRITE) {
		allow_dirs = tctx->write_allowed_dirs;
		allow_count = &tctx->write_allowed_dirs_count;
	} else {
		allow_dirs = tctx->read_allowed_dirs;
		allow_count = &tctx->read_allowed_dirs_count;
	}
	if (!tctx->operation_approval_fn)
		MORPH_RETURN(-EPERM);
	v = tctx->operation_approval_fn(op,
					tctx->operation_approval_user_data);
	if (v == TOOL_OP_DENY)
		MORPH_RETURN(-EACCES);
	if (v == TOOL_OP_SESSION || v == TOOL_OP_ALWAYS)
		add_parent_dir(allow_dirs, allow_count, op->target);
	if (v == TOOL_OP_ALWAYS) {
		char parent[PATH_MAX];
		char *slash;
		const char *kind = op->kind == TOOL_OP_PATH_WRITE ?
			"tool_write_path" : "read_path";

		snprintf(parent, sizeof(parent), "%s", op->target);
		slash = strrchr(parent, '/');
		if (slash && slash != parent)
			*slash = '\0';
		(void)save_grant(tctx,
				 op->principal ? op->principal : "tool:*",
				 kind, parent);
	}
	return 0;
}

static int scoped_write_is_allowed(const struct tool_context *tctx,
				   const char *principal, const char *path)
{
	struct tool_scoped_grant *grant;

	morph_array_foreach(grant, &tctx->scoped_grants,
			    struct tool_scoped_grant) {
		if (strcmp(grant->subject, principal) == 0 &&
		    path_is_within(path, grant->path))
			return 1;
	}
	return 0;
}

int tool_context_request_write_access(struct tool_context *tctx,
				      const char *principal,
				      const char *command,
				      const char *path,
				      char *resolved,
				      size_t resolved_size)
{
	char local[PATH_MAX];
	char *expanded;
	char *canonical;
	struct tool_operation op;
	enum tool_operation_verdict verdict;

	if (!tctx || !principal || !*principal || !path || !*path)
		MORPH_RETURN(-EINVAL);
	if (!resolved || resolved_size == 0) {
		resolved = local;
		resolved_size = sizeof(local);
	}
	expanded = file_expand_path(path);
	if (!expanded)
		MORPH_RETURN(-ENOMEM);
	if (!file_path_is_absolute(expanded)) {
		free(expanded);
		MORPH_RETURN(-EINVAL);
	}
	canonical = file_resolve_path(expanded);
	free(expanded);
	if (!canonical)
		MORPH_RETURN(-ENOMEM);
	snprintf(resolved, resolved_size, "%s", canonical);
	free(canonical);

	if ((tctx->output_dir[0] &&
	     path_is_within(resolved, tctx->output_dir)) ||
	    scoped_write_is_allowed(tctx, principal, resolved))
		return 0;
	if (!tctx->operation_approval_fn)
		MORPH_RETURN(-EPERM);
	memset(&op, 0, sizeof(op));
	op.kind = TOOL_OP_PATH_WRITE;
	op.tool_name = "bash_exec";
	op.principal = principal;
	op.action = command;
	op.target = resolved;
	op.scope = tctx->grant_project_root;
	verdict = tctx->operation_approval_fn(
		&op, tctx->operation_approval_user_data);
	if (verdict == TOOL_OP_DENY)
		MORPH_RETURN(-EACCES);
	if (verdict == TOOL_OP_SESSION || verdict == TOOL_OP_ALWAYS) {
		int rc = add_scoped_write_grant(tctx, principal, resolved);
		if (rc != 0)
			return rc;
		if (verdict == TOOL_OP_ALWAYS)
			return save_grant(tctx, principal, "write_path",
					  resolved);
	}
	return 0;
}

int tool_context_collect_write_grants(const struct tool_context *tctx,
				      const char *principal,
				      const char **paths, int max_paths)
{
	struct tool_scoped_grant *grant;
	int count = 0;

	if (!tctx || !principal || !paths || max_paths <= 0)
		return 0;
	morph_array_foreach(grant, &tctx->scoped_grants,
			    struct tool_scoped_grant) {
		if (strcmp(grant->subject, principal) != 0)
			continue;
		if (count >= max_paths)
			break;
		paths[count++] = grant->path;
	}
	return count;
}

int tool_context_check_operation(struct tool_context *tctx,
				 const struct tool_operation *op)
{
	if (!tctx || !op)
		MORPH_RETURN(-EINVAL);
	switch (op->kind) {
	case TOOL_OP_COMMAND:
		return check_command_operation(tctx, op);
	case TOOL_OP_PATH_READ:
	case TOOL_OP_PATH_LIST:
	case TOOL_OP_PATH_WRITE:
		return check_path_operation(tctx, op);
	case TOOL_OP_NETWORK:
	case TOOL_OP_EXTERNAL_SEND:
		if (!tctx->operation_approval_fn)
			MORPH_RETURN(-EPERM);
		if (tctx->operation_approval_fn(
			    op, tctx->operation_approval_user_data) ==
		    TOOL_OP_DENY)
			MORPH_RETURN(-EACCES);
		return 0;
	}
	MORPH_RETURN(-EINVAL);
}
