#include "context.h"
#include "context_internal.h"
#include "path.h"
#include "trusted_paths.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "db/permission_grant.h"
#include "nb/file.h"
#include "nb/log.h"
#include "errors.h"
#include "permission/command_analysis.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#define TOOL_GRANT_SUBJECT_MAX 256

static int review_user(const struct tool_operation *op,
	struct permission_review *review, void *opaque)
{
	struct tool_context *tctx = opaque;

	if (!tctx->operation_approval_fn)
		return NB_ERR_NOT_PERMITTED;
	review->verdict = tctx->operation_approval_fn(op,
		tctx->operation_approval_user_data);
	return 0;
}

static int tool_context_review_locked(struct tool_context *tctx,
	const struct tool_operation *op, enum tool_operation_verdict *verdict)
{
	if (!tctx || !op)
		return NB_ERR_INVAL;
	return permission_authorize(&tctx->permissions, op,
		permission_check(&tctx->permissions.policy, op, 0), verdict);
}

static int tool_context_set_permission_policy_locked(struct tool_context *tctx,
	const struct permission_policy *policy)
{
	if (!tctx || permission_policy_validate(policy) != 0)
		return NB_ERR_INVAL;
	tctx->permissions.policy = *policy;
	tctx->permissions.consecutive_denials = 0;
	nb_vec_clear(&tctx->scoped_grants);
	tctx->read_allowed_dirs_count = 0;
	tctx->write_allowed_dirs_count = 0;
	tctx->delete_allowed_dirs_count = 0;
	tctx->allowed_commands_count = 0;
	tctx->tool_calls_count = 0;
	tctx->exec_allowed_dirs_count = 0;
	return 0;
}

static int read_only(const struct tool_context *tctx)
{
	return tctx->permissions.policy.access == PERMISSION_READ_ONLY;
}

static const char *grant_kind(const struct tool_context *tctx, const char *kind)
{
	if (!read_only(tctx))
		return kind;
	if (!strcmp(kind, "write_path")) return "readonly_write_path";
	if (!strcmp(kind, "tool_write_path")) return "readonly_tool_write_path";
	if (!strcmp(kind, "delete_path")) return "readonly_delete_path";
	if (!strcmp(kind, "tool_call")) return "readonly_tool_call";
	return kind;
}

static int configured_path_allowed(const char *path,
	const char dirs[][TOOL_CONTEXT_ALLOW_PATH_MAX], int count);

struct tool_builtin_path {
	char *path;
	unsigned int operations;
	int directory;
};

struct tool_scoped_grant {
	char subject[TOOL_GRANT_SUBJECT_MAX];
	char path[PATH_MAX];
	enum tool_path_op kind;
	int turn_only;
};

static int append_persistent_grant(nb_vec *grants, const struct permission_grant *grant)
{
	struct permission_grant *existing;
	struct permission_grant *stored;

	if (!grants || !grant)
		return NB_ERR_INVAL;
	for (size_t _i_ = 0; _i_ < nb_vec_len(grants) &&
			     (existing = (struct permission_grant *)nb_vec_at(grants, _i_));
	     _i_++) {
		if ((grant->id > 0 && existing->id == grant->id) ||
		    (strcmp(existing->subject, grant->subject) == 0 &&
		     strcmp(existing->resource_kind,
			    grant->resource_kind) == 0 &&
		     strcmp(existing->resource, grant->resource) == 0))
			return 0;
	}
	stored = nb_vec_push(grants);
	if (!stored)
		return NB_ERR_NOMEM;
	*stored = *grant;
	return 0;
}

static int add_persistent_grant(struct tool_context *tctx,
				const struct permission_grant *grant)
{
	if (!tctx)
		return NB_ERR_INVAL;
	return append_persistent_grant(&tctx->persistent_grants, grant);
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

static int command_matches_pattern(const char *cmd, const char *pat)
{
	char name[TOOL_CONTEXT_CLI_NAME_MAX];

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
		if (!has_whitespace(pat) &&
		    tool_context_command_name(cmd, name, sizeof(name)) == 0)
			return strlen(name) == base &&
				strncmp(name, pat, base) == 0;
		if (strncmp(cmd, pat, base) != 0)
			return 0;
		char after = cmd[base];
		return after == '\0' || after == ' ' || after == '\t';
	}
	if (!has_whitespace(pat)) {
		if (tool_context_command_name(
			    cmd, name, sizeof(name)) != 0)
			return 0;
		return strcmp(name, pat) == 0;
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

static int command_program_word(const char *command, char *out,
				size_t out_size)
{
	return bash_parse_command_program(command, out, out_size);
}

int tool_context_command_name(const char *command, char *out,
			      size_t out_size)
{
	return bash_parse_command_name(command, out, out_size);
}

int tool_context_command_principal(const char *command, char *out,
				   size_t out_size)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_2 = NB_OK;
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};

	char program[PATH_MAX];
	const char *path;
	int rc;

	if (!command || !out || out_size == 0)
		return NB_ERR_INVAL;
	rc = command_program_word(command, program, sizeof(program));
	if (rc != 0)
		return rc;
	if (strchr(program, '/')) {
		char *resolved = (nb_path_expand(&file_value_1, program, NULL) == NB_OK &&
						  nb_path_canonical_missing(
							  &file_value_1, nb_buf_cstr(&file_value_1),
							  NULL) == NB_OK
					  ? strdup(nb_buf_cstr(&file_value_1))
					  : NULL);
		if (!resolved)
			return NB_ERR_NOT_FOUND;
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
			return NB_ERR_NOMEM;
		for (dir = strtok_r(copy, ":", &save); dir;
		     dir = strtok_r(NULL, ":", &save)) {
			char candidate[PATH_MAX];
			char *resolved;

			if (((file_status_2 = nb_path_concat(&file_value_2, dir, program)) != NB_OK
				     ? file_status_2
			     : (file_status_2 = nb_str_copy_to(nb_buf_str(&file_value_2), candidate,
							       sizeof(candidate))) == NB_ERR_RANGE
				     ? NB_ERR_NAME_TOO_LONG
				     : file_status_2) != 0 ||
			    access(candidate, X_OK) != 0)
				continue;
			resolved =
				(nb_path_expand(&file_value_3, candidate, NULL) == NB_OK &&
						 nb_path_canonical_missing(
							 &file_value_3, nb_buf_cstr(&file_value_3),
							 NULL) == NB_OK
					 ? strdup(nb_buf_cstr(&file_value_3))
					 : NULL);
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
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_5 NB_CLEANUP(nb_buf_destroy) = {0};

	if (!src || !*src) {
		dst[0] = '\0';
		return;
	}
	char *resolved =
		(nb_path_expand(&file_value_4, src, NULL) == NB_OK &&
				 nb_path_canonical_missing(
					 &file_value_4, nb_buf_cstr(&file_value_4), NULL) == NB_OK
			 ? strdup(nb_buf_cstr(&file_value_4))
			 : NULL);
	if (resolved) {
		strncpy(dst, resolved, dst_size - 1);
		dst[dst_size - 1] = '\0';
		free(resolved);
	} else {
		char *expanded = (nb_path_expand(&file_value_5, src, NULL) == NB_OK
					  ? strdup(nb_buf_cstr(&file_value_5))
					  : NULL);
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
	case TOOL_OP_PATH_DELETE:
		return "delete";
	case TOOL_OP_DIRECTORY_ACCESS:
		return "directory_access";
	case TOOL_OP_TOOL_CALL:
		return "tool_call";
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
	case TOOL_PATH_DELETE:
		return TOOL_OP_PATH_DELETE;
	}
	return TOOL_OP_PATH_READ;
}

static int resolve_user_path(struct tool_context *tctx,
			     enum tool_path_op op,
			     const char *path,
			     char *resolved,
			     size_t resolved_size)
{
	nb_buf file_value_6 NB_CLEANUP(nb_buf_destroy) = {0};
	int expand_status_6 = NB_OK;
	nb_buf file_value_7 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_7 = NB_OK;
	nb_buf file_value_8 NB_CLEANUP(nb_buf_destroy) = {0};

	char candidate[PATH_MAX];
	const char *base;
	char *expanded;
	char *rp;

	if (!tctx || !path || !resolved || resolved_size == 0)
		return NB_ERR_INVAL;

	expanded = ((expand_status_6 = nb_path_expand(&file_value_6, path, NULL)) == NB_OK
			    ? strdup(nb_buf_cstr(&file_value_6))
			    : NULL);
	if (!expanded)
		return expand_status_6 != NB_OK ? expand_status_6 : NB_ERR_NOMEM;

	base = path_op_is_read(op) ? tctx->workdir : tctx->output_dir;
	if ((!base || !*base) && !nb_path_is_absolute(expanded))
		base = "";
	if (((file_status_7 = nb_path_join(&file_value_7, base, expanded)) != NB_OK ? file_status_7
	     : (file_status_7 = nb_str_copy_to(nb_buf_str(&file_value_7), candidate,
					       sizeof(candidate))) == NB_ERR_RANGE
		     ? NB_ERR_NAME_TOO_LONG
		     : file_status_7) != 0) {
		free(expanded);
		return NB_ERR_NAME_TOO_LONG;
	}
	free(expanded);

	rp = (nb_path_expand(&file_value_8, candidate, NULL) == NB_OK &&
			      nb_path_canonical_missing(&file_value_8, nb_buf_cstr(&file_value_8),
							NULL) == NB_OK
		      ? strdup(nb_buf_cstr(&file_value_8))
		      : NULL);
	if (!rp)
		return NB_ERR_NOT_FOUND;
	snprintf(resolved, resolved_size, "%s", rp);
	free(rp);
	return 0;
}

static int add_allowed_dir(char dirs[][TOOL_CONTEXT_ALLOW_PATH_MAX],
			   int *count, const char *dir)
{
	nb_buf file_value_9 NB_CLEANUP(nb_buf_destroy) = {0};

	char *resolved;
	const char *to_store;

	if (!dirs || !count || !dir)
		return NB_ERR_INVAL;
	resolved =
		(nb_path_expand(&file_value_9, dir, NULL) == NB_OK &&
				 nb_path_canonical_missing(
					 &file_value_9, nb_buf_cstr(&file_value_9), NULL) == NB_OK
			 ? strdup(nb_buf_cstr(&file_value_9))
			 : NULL);
	to_store = resolved ? resolved : dir;
	for (int i = 0; i < *count; i++) {
		if (strcmp(dirs[i], to_store) == 0) {
			free(resolved);
			return 0;
		}
	}
	if (*count >= TOOL_CONTEXT_ALLOW_MAX) {
		free(resolved);
		return NB_ERR_NO_SPACE;
	}
	if (strlen(to_store) >= TOOL_CONTEXT_ALLOW_PATH_MAX) {
		free(resolved);
		return NB_ERR_NAME_TOO_LONG;
	}
	snprintf(dirs[*count], TOOL_CONTEXT_ALLOW_PATH_MAX, "%s", to_store);
	(*count)++;
	free(resolved);
	return 0;
}

static int add_scoped_path_grant(struct tool_context *tctx,
				 enum tool_path_op kind,
				 const char *subject, const char *path,
				 int turn_only)
{
	struct tool_scoped_grant grant;
	struct tool_scoped_grant *existing;

	if (!tctx || !subject || !*subject || !path || !*path)
		return NB_ERR_INVAL;
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&tctx->scoped_grants) &&
	     (existing = (struct tool_scoped_grant *)nb_vec_at(&tctx->scoped_grants, _i_));
	     _i_++) {
		if (existing->kind == kind &&
		    strcmp(existing->subject, subject) == 0 &&
		    strcmp(existing->path, path) == 0) {
			if (!turn_only)
				existing->turn_only = 0;
			return 0;
		}
	}
	memset(&grant, 0, sizeof(grant));
	snprintf(grant.subject, sizeof(grant.subject), "%s", subject);
	snprintf(grant.path, sizeof(grant.path), "%s", path);
	grant.kind = kind;
	grant.turn_only = !!turn_only;
	existing = nb_vec_push(&tctx->scoped_grants);
	if (!existing)
		return NB_ERR_NOMEM;
	*existing = grant;
	return 0;
}

static int path_check_for_read(enum tool_path_op op, const char *path)
{
	struct stat st;

	if (!path_op_is_read(op))
		return 0;
	if (stat(path, &st) != 0)
		return nb_status_from_errno(errno, NULL);
	if (op == TOOL_PATH_LIST && !S_ISDIR(st.st_mode))
		return NB_ERR_NOT_DIR;
	return 0;
}

struct tool_context *tool_context_create(struct nb_log *logger, const char *workdir,
					 const char *output_dir)
{
	struct tool_context *tctx = calloc(1, sizeof(*tctx));
	if (!tctx)
		return NULL;
	if (permission_context_init(&tctx->permissions) != 0) {
		free(tctx);
		return NULL;
	}
	tctx->authority_lock = nb_mutex_new(NB_MUTEX_RECURSIVE);
	int lock_rc = tctx->authority_lock ? NB_OK : NB_ERR_NOMEM;
	if (lock_rc != 0) {
		permission_context_cleanup(&tctx->permissions);
		free(tctx);
		return NULL;
	}
	tctx->permissions.user_review = review_user;
	tctx->permissions.user_data = tctx;
	resolve_into(tctx->workdir, sizeof(tctx->workdir), workdir);
	resolve_into(tctx->output_dir, sizeof(tctx->output_dir), output_dir);
	tctx->logger = logger;
	tctx->operation_approval_fn = NULL;
	tctx->operation_approval_user_data = NULL;
	tctx->read_allowed_dirs_count = 0;
	tctx->write_allowed_dirs_count = 0;
	if (tctx->output_dir[0] &&
	    !(tctx->workdir[0] && nb_path_is_within(tctx->output_dir, tctx->workdir, true))) {
		add_allowed_dir(tctx->read_allowed_dirs,
				&tctx->read_allowed_dirs_count,
				tctx->output_dir);
	}
	tctx->allowed_commands_count = 0;
	tctx->exec_allowed_dirs_count = 0;
	if ((nb_vec_init(&tctx->scoped_grants, sizeof(struct tool_scoped_grant)),
	     nb_vec_reserve(&tctx->scoped_grants, 8) ? 0 : NB_ERR_NOMEM) != 0) {
		(nb_mutex_free(tctx->authority_lock), tctx->authority_lock = NULL);
		permission_context_cleanup(&tctx->permissions);
		free(tctx);
		return NULL;
	}
	if ((nb_vec_init(&tctx->persistent_grants, sizeof(struct permission_grant)),
	     nb_vec_reserve(&tctx->persistent_grants, 8) ? 0 : NB_ERR_NOMEM) != 0) {
		nb_vec_cleanup(&tctx->scoped_grants);
		(nb_mutex_free(tctx->authority_lock), tctx->authority_lock = NULL);
		permission_context_cleanup(&tctx->permissions);
		free(tctx);
		return NULL;
	}
	if ((nb_vec_init(&tctx->builtin_paths, sizeof(struct tool_builtin_path)),
	     nb_vec_reserve(&tctx->builtin_paths, 32) ? 0 : NB_ERR_NOMEM) != 0) {
		nb_vec_cleanup(&tctx->scoped_grants);
		nb_vec_cleanup(&tctx->persistent_grants);
		(nb_mutex_free(tctx->authority_lock), tctx->authority_lock = NULL);
		permission_context_cleanup(&tctx->permissions);
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
	if (tctx) {
		struct tool_builtin_path *entry;

		for (size_t _i_ = 0;
		     _i_ < nb_vec_len(&tctx->builtin_paths) &&
		     (entry = (struct tool_builtin_path *)nb_vec_at(&tctx->builtin_paths, _i_));
		     _i_++)
			free(entry->path);
		nb_vec_cleanup(&tctx->builtin_paths);
		nb_vec_cleanup(&tctx->scoped_grants);
		nb_vec_cleanup(&tctx->persistent_grants);
		(nb_mutex_free(tctx->authority_lock), tctx->authority_lock = NULL);
		permission_context_cleanup(&tctx->permissions);
	}
	free(tctx);
}

const char *tool_context_workdir_borrow(const struct tool_context *tctx)
{
	if (!tctx)
		return NULL;
	return tctx->workdir;
}

const char *tool_context_output_dir_borrow(const struct tool_context *tctx)
{
	if (!tctx)
		return NULL;
	return tctx->output_dir;
}

static void tool_context_set_default_timeout_locked(struct tool_context *tctx, int seconds)
{
	if (!tctx)
		return;
	tctx->default_timeout_seconds = seconds > 0 ? seconds : 0;
}

static int tool_context_default_timeout_locked(const struct tool_context *tctx)
{
	if (!tctx)
		return 0;
	return tctx->default_timeout_seconds;
}

static void tool_context_add_read_allowed_dir_locked(struct tool_context *tctx,
				       const char *dir)
{
	if (!tctx)
		return;
	add_allowed_dir(tctx->read_allowed_dirs,
			&tctx->read_allowed_dirs_count, dir);
}

static void tool_context_add_write_allowed_dir_locked(struct tool_context *tctx,
					const char *dir)
{
	if (!tctx)
		return;
	add_allowed_dir(tctx->write_allowed_dirs,
			&tctx->write_allowed_dirs_count, dir);
}

static void tool_context_set_operation_approval_locked(struct tool_context *tctx,
					 tool_operation_approval_fn fn,
					 void *user_data)
{
	if (!tctx)
		return;
	tctx->operation_approval_fn = fn;
	tctx->operation_approval_user_data = user_data;
}

struct grant_load_state {
	nb_vec grants;
};

static int load_grant(const struct permission_grant *grant, void *user_data)
{
	struct grant_load_state *state = user_data;

	return append_persistent_grant(&state->grants, grant);
}

static int tool_context_reload_persistent_grants_locked(struct tool_context *tctx)
{
	struct grant_load_state state;
	int rc;

	if (!tctx || !tctx->grant_db || !tctx->grant_project_root[0])
		return NB_ERR_INVAL;
	rc = (nb_vec_init(&state.grants, sizeof(struct permission_grant)),
	      nb_vec_reserve(&state.grants, 8) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = permission_grant_each(tctx->grant_db, tctx->grant_project_root,
				   load_grant, &state);
	if (rc != 0) {
		nb_vec_cleanup(&state.grants);
		return rc;
	}
	/* Keep the previous grants when the reload fails part-way. */
	nb_vec_cleanup(&tctx->persistent_grants);
	tctx->persistent_grants = state.grants;
	return 0;
}

static int tool_context_set_grant_store_locked(struct tool_context *tctx, struct db *db,
				 const char *project_root)
{
	nb_buf file_value_10 NB_CLEANUP(nb_buf_destroy) = {0};
	int expand_status_10 = NB_OK;

	char *resolved;
	int rc;

	if (!tctx || !db || !project_root || !*project_root)
		return NB_ERR_INVAL;
	resolved =
		((expand_status_10 = nb_path_expand(&file_value_10, project_root, NULL)) == NB_OK &&
				 (expand_status_10 = nb_path_canonical_missing(
					 &file_value_10, nb_buf_cstr(&file_value_10),
						NULL)) == NB_OK
			 ? strdup(nb_buf_cstr(&file_value_10))
			 : NULL);
	if (!resolved)
		return expand_status_10 != NB_OK ? expand_status_10 : NB_ERR_NOMEM;
	snprintf(tctx->grant_project_root, sizeof(tctx->grant_project_root),
		 "%s", resolved);
	free(resolved);
	tctx->grant_db = db;
	rc = tool_context_reload_persistent_grants(tctx);
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
	int rc;

	if (!tctx || !kind || !*kind || !resource || !*resource)
		return NB_ERR_INVAL;
	if ((subject && strlen(subject) >= sizeof(grant.subject)) ||
	    strlen(grant_kind(tctx, kind)) >= sizeof(grant.resource_kind) ||
	    strlen(resource) >= sizeof(grant.resource))
		return NB_ERR_NAME_TOO_LONG;
	memset(&grant, 0, sizeof(grant));
	snprintf(grant.subject, sizeof(grant.subject), "%s",
		 subject ? subject : "*");
	snprintf(grant.resource_kind, sizeof(grant.resource_kind), "%s", grant_kind(tctx, kind));
	snprintf(grant.resource, sizeof(grant.resource), "%s", resource);
	if (tctx->grant_project_root[0])
		snprintf(grant.project_root, sizeof(grant.project_root), "%s",
			 tctx->grant_project_root);
	if (tctx->grant_db && grant.project_root[0]) {
		rc = permission_grant_save(tctx->grant_db, &grant);
		if (rc != 0)
			return rc;
	}
	return add_persistent_grant(tctx, &grant);
}

static int persistent_path_is_allowed(const struct tool_context *tctx,
	const char *subject, const char *kind, const char *path)
{
	struct permission_grant *grant;

	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&tctx->persistent_grants) &&
	     (grant = (struct permission_grant *)nb_vec_at(&tctx->persistent_grants, _i_));
	     _i_++) {
		if (strcmp(grant->resource_kind, grant_kind(tctx, kind)) == 0 &&
		    (strcmp(grant->subject, subject) == 0 || strcmp(grant->subject, "*") == 0) &&
		    nb_path_is_within(path, grant->resource, true))
			return 1;
	}
	return 0;
}

static int tool_context_add_builtin_path_locked(struct tool_context *tctx,
				 enum tool_path_op op, const char *path)
{
	nb_buf file_value_11 NB_CLEANUP(nb_buf_destroy) = {0};
	int expand_status_11 = NB_OK;

	struct tool_builtin_path *entry;
	struct stat st;
	char *canonical;
	unsigned int operations;

	if (!tctx || !path || op < TOOL_PATH_READ || op > TOOL_PATH_DELETE)
		return NB_ERR_INVAL;
	if (stat(path, &st) != 0)
		return nb_status_from_errno(errno, NULL);
	canonical =
		((expand_status_11 = nb_path_expand(&file_value_11, path, NULL)) == NB_OK &&
				 (expand_status_11 = nb_path_canonical_missing(
					 &file_value_11, nb_buf_cstr(&file_value_11),
						NULL)) == NB_OK
			 ? strdup(nb_buf_cstr(&file_value_11))
			 : NULL);
	if (!canonical)
		return expand_status_11 != NB_OK ? expand_status_11 : NB_ERR_NOMEM;
	operations = 1u << (unsigned int)op;
	if (op == TOOL_PATH_READ && S_ISDIR(st.st_mode))
		operations |= 1u << TOOL_PATH_LIST;
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&tctx->builtin_paths) &&
	     (entry = (struct tool_builtin_path *)nb_vec_at(&tctx->builtin_paths, _i_));
	     _i_++) {
		if (strcmp(entry->path, canonical) == 0) {
			entry->operations |= operations;
			free(canonical);
			return 0;
		}
	}
	entry = nb_vec_push(&tctx->builtin_paths);
	if (!entry) {
		free(canonical);
		return NB_ERR_NOMEM;
	}
	entry->path = canonical;
	entry->operations = operations;
	entry->directory = S_ISDIR(st.st_mode);
	return 0;
}

static int builtin_path_is_allowed(const struct tool_context *tctx,
				   enum tool_path_op op, const char *path)
{
	struct tool_builtin_path *entry;

	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&tctx->builtin_paths) &&
	     (entry = (struct tool_builtin_path *)nb_vec_at(&tctx->builtin_paths, _i_));
	     _i_++) {
		size_t len = strlen(entry->path);

		/* Both paths are canonical; do not follow a retargeted root. */
		if (!(entry->operations & (1u << (unsigned int)op)))
			continue;
		if (strcmp(path, entry->path) == 0 ||
		    (entry->directory && strncmp(path, entry->path, len) == 0 &&
		     path[len] == '/'))
			return 1;
	}
	return 0;
}

static int directory_grant_is_allowed(const struct tool_context *tctx,
				      enum tool_path_op op, const char *path,
				      int session_scope)
{
	struct tool_scoped_grant *grant;
	const char *kind;

	if (path_op_is_read(op))
		op = TOOL_PATH_READ;
	kind = op == TOOL_PATH_READ ? "read_path" :
		op == TOOL_PATH_WRITE ? "write_path" : "delete_path";
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&tctx->scoped_grants) &&
	     (grant = (struct tool_scoped_grant *)nb_vec_at(&tctx->scoped_grants, _i_));
	     _i_++) {
		if (grant->kind == op && (!session_scope || !grant->turn_only) &&
		    strcmp(grant->subject, "*") == 0 && nb_path_is_within(path, grant->path, true))
			return 1;
	}
	return persistent_path_is_allowed(tctx, "*", kind, path);
}

static int policy_path_is_allowed(const struct tool_context *tctx,
				  enum tool_path_op op, const char *path)
{
	if (tctx->permissions.policy.access == PERMISSION_FULL)
		return 1;
	if (directory_grant_is_allowed(tctx, op, path, 0))
		return 1;
	if (read_only(tctx) && !path_op_is_read(op))
		return 0;
	if (builtin_path_is_allowed(tctx, op, path))
		return 1;
	if (tctx->workdir[0] && nb_path_is_within(path, tctx->workdir, true))
		return 1;
	if (path_op_is_read(op) && tctx->output_dir[0] &&
	    nb_path_is_within(path, tctx->output_dir, true))
		return 1;
	if (op != TOOL_PATH_DELETE && configured_path_allowed(path,
			tctx->exec_profile_write_dirs, tctx->exec_profile_write_dirs_count))
		return 1;
	if (op != TOOL_PATH_WRITE && configured_path_allowed(path,
			tctx->exec_profile_delete_dirs, tctx->exec_profile_delete_dirs_count))
		return 1;
	return 0;
}

static int tool_context_authorize_path_locked(struct tool_context *tctx,
				enum tool_path_op op, const char *path,
				char *resolved, size_t resolved_size)
{
	char path_buf[PATH_MAX];
	const char *root;
	char (*allow_dirs)[TOOL_CONTEXT_ALLOW_PATH_MAX];
	int *allow_count;
	int rc;
	struct tool_operation operation;

	if (!tctx || !path || op < TOOL_PATH_READ || op > TOOL_PATH_DELETE)
		return NB_ERR_INVAL;
	if (!resolved || resolved_size == 0) {
		resolved = path_buf;
		resolved_size = sizeof(path_buf);
	}

	rc = resolve_user_path(tctx, op, path, resolved, resolved_size);
	if (rc < 0)
		return rc;

	rc = path_check_for_read(op, resolved);
	if (rc < 0)
		return rc;

	if (policy_path_is_allowed(tctx, op, resolved))
		return 0;

	root = path_op_is_read(op) ? tctx->workdir : tctx->output_dir;
	allow_dirs = path_op_is_read(op) ? tctx->read_allowed_dirs :
		op == TOOL_PATH_DELETE ? tctx->delete_allowed_dirs : tctx->write_allowed_dirs;
	allow_count = path_op_is_read(op) ? &tctx->read_allowed_dirs_count :
		op == TOOL_PATH_DELETE ? &tctx->delete_allowed_dirs_count :
		&tctx->write_allowed_dirs_count;

	if ((!read_only(tctx) || path_op_is_read(op)) && root && *root &&
	    nb_path_is_within(resolved, root, true))
		return 0;
	for (int i = 0; i < *allow_count; i++) {
		if (nb_path_is_within(resolved, allow_dirs[i], true))
			return 0;
	}
	if (persistent_path_is_allowed(tctx, "tool:*",
			path_op_is_read(op) ? "read_path" :
			op == TOOL_PATH_DELETE ? "delete_path" : "tool_write_path", resolved))
		return 0;

	memset(&operation, 0, sizeof(operation));
	operation.kind = path_op_to_kind(op);
	operation.tool_name = "tool";
	operation.principal = "tool:*";
	operation.target = resolved;
	operation.scope = root;

	rc = tool_context_check_operation(tctx, &operation);
	if (rc == 0)
		return 0;

	nb_log_write(tctx ? tctx->logger : NULL, NB_LOG_WARN, "%s path denied (outside root): %s",
		     operation_kind_name(path_op_to_kind(op)), resolved);
	return rc;
}

static int tool_context_allow_command_pattern_locked(struct tool_context *tctx,
				       const char *pattern)
{
	if (!tctx || !pattern || !*pattern)
		return NB_ERR_INVAL;
	if (strlen(pattern) >= TOOL_CONTEXT_ACTION_MAX)
		return NB_ERR_NAME_TOO_LONG;
	for (int i = 0; i < tctx->allowed_commands_count; i++) {
		if (strcmp(tctx->allowed_commands[i], pattern) == 0)
			return 0;
	}
	if (tctx->allowed_commands_count >= TOOL_CONTEXT_ALLOW_MAX)
		return NB_ERR_NO_SPACE;
	snprintf(tctx->allowed_commands[tctx->allowed_commands_count],
		 TOOL_CONTEXT_ACTION_MAX, "%s", pattern);
	tctx->allowed_commands_count++;
	return 0;
}

static int tool_context_allow_command_scope_locked(struct tool_context *tctx, const char *path)
{
	nb_buf resolved NB_CLEANUP(nb_buf_destroy) = {0};
	const char *stored;

	if (!tctx || !path || !*path)
		return NB_ERR_INVAL;
	if (strcmp(path, "*") == 0) {
		stored = "*";
	} else {
		int rc = nb_path_canonical(&resolved, path, NULL);
		if (rc != NB_OK)
			return rc;
		stored = nb_buf_cstr(&resolved);
	}
	for (int i = 0; i < tctx->exec_allowed_dirs_count; i++) {
		if (strcmp(tctx->exec_allowed_dirs[i], stored) == 0)
			return 0;
	}
	if (tctx->exec_allowed_dirs_count >= TOOL_CONTEXT_ALLOW_MAX)
		return NB_ERR_NO_SPACE;
	snprintf(tctx->exec_allowed_dirs[tctx->exec_allowed_dirs_count],
		 TOOL_CONTEXT_ALLOW_PATH_MAX, "%s", stored);
	tctx->exec_allowed_dirs_count++;
	return 0;
}

static int command_is_allowed(struct tool_context *tctx, const char *command)
{
	struct permission_grant *grant;

	for (int i = 0; i < tctx->allowed_commands_count; i++) {
		if (command_matches_pattern(command,
					    tctx->allowed_commands[i]))
			return 1;
	}
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&tctx->persistent_grants) &&
	     (grant = (struct permission_grant *)nb_vec_at(&tctx->persistent_grants, _i_));
	     _i_++) {
		if (strcmp(grant->resource_kind, "command") == 0 &&
		    command_matches_pattern(command, grant->resource))
			return 1;
	}
	return 0;
}

static int command_scope_is_allowed(struct tool_context *tctx, const char *cwd)
{
	struct permission_grant *grant;

	if (cwd && *cwd) {
		nb_buf resolved NB_CLEANUP(nb_buf_destroy) = {0};
		if (nb_path_canonical(&resolved, cwd, NULL) == NB_OK) {
			for (int i = 0; i < tctx->exec_allowed_dirs_count; i++) {
				if (cwd_matches_pattern(
					    nb_buf_cstr(&resolved),
					    tctx->exec_allowed_dirs[i]))
					return 1;
			}
			for (size_t _i_ = 0; _i_ < nb_vec_len(&tctx->persistent_grants) &&
					     (grant = (struct permission_grant *)nb_vec_at(
						      &tctx->persistent_grants, _i_));
			     _i_++) {
				nb_buf grant_path NB_CLEANUP(nb_buf_destroy) = {0};
				const char *pattern = grant->resource;

				if (nb_path_canonical(&grant_path, grant->resource, NULL) == NB_OK)
					pattern = nb_buf_cstr(&grant_path);
				if (strcmp(grant->resource_kind,
					   "command_cwd") == 0 &&
				    cwd_matches_pattern(nb_buf_cstr(&resolved),
							pattern))
					return 1;
			}
		}
		return 0;
	}
	return 1;
}

static int operation_programs_allowed(struct tool_context *tctx,
				      const struct tool_operation *op)
{
	if (op->programs && op->programs_count > 0) {
		for (int i = 0; i < op->programs_count; i++) {
			if (!command_is_allowed(tctx, op->programs[i]))
				return 0;
		}
		return 1;
	}
	return command_is_allowed(tctx, op->action);
}

static int persist_command_programs(struct tool_context *tctx,
				     const struct tool_operation *op,
				     enum tool_operation_verdict verdict)
{
	int count = op->programs && op->programs_count > 0 ?
		op->programs_count : 1;

	for (int i = 0; i < count; i++) {
		char program[TOOL_CONTEXT_ACTION_MAX];
		const char *command = op->programs && op->programs_count > 0 ?
			op->programs[i] : op->action;
		int rc;

		rc = tool_context_command_name(command, program, sizeof(program));
		if (rc != 0)
			return rc;
		rc = verdict == TOOL_OP_ALWAYS ?
			save_grant(tctx, program, "command", program) :
			tool_context_allow_command_pattern(tctx, program);
		if (rc < 0)
			return rc;
		else
			nb_log_write(tctx ? tctx->logger : NULL, NB_LOG_INFO,
				     "persisted command '%s' %s", program,
				     verdict == TOOL_OP_ALWAYS ? "always" : "for session");
	}
	return 0;
}

static int check_command_operation(struct tool_context *tctx,
				   const struct tool_operation *op,
				   enum tool_operation_verdict *verdict)
{
	const char *command = op->action;
	const char *cwd = op->scope;
	struct tool_operation approval_operation;
	nb_buf resolved_cwd NB_CLEANUP(nb_buf_destroy) = {0};
	int cmd_ok;
	int cwd_ok;
	enum tool_operation_verdict v;

	if (!command)
		return NB_ERR_INVAL;
	if (!op->host && !op->network && !op->require_review &&
	    !op->require_user && !op->forbidden && cwd &&
	    nb_path_canonical(&resolved_cwd, cwd, NULL) == NB_OK) {
		if ((tctx->workdir[0] && nb_path_is_within(nb_buf_cstr(&resolved_cwd),
			tctx->workdir, true)) ||
		    permission_temporary_path_is_default(nb_buf_cstr(&resolved_cwd))) {
			*verdict = TOOL_OP_ALLOW;
			return 0;
		}
	}

	cmd_ok = operation_programs_allowed(tctx, op);
	cwd_ok = command_scope_is_allowed(tctx, cwd);
	if (cmd_ok && cwd_ok && !op->host && !op->network &&
	    !op->require_review && !op->require_user && !op->forbidden) {
		*verdict = TOOL_OP_ALLOW;
		return 0;
	}
	approval_operation = *op;
	if (!approval_operation.reason) {
		if (!cmd_ok && !cwd_ok)
			approval_operation.reason =
				"Programs and working directory are outside "
				"the current trusted scope.";
		else if (!cmd_ok)
			approval_operation.reason =
				"One or more programs are not trusted yet.";
		else
			approval_operation.reason =
				"The working directory is outside the trusted "
				"command scope.";
	}
	int approval_rc = tool_context_review(tctx, &approval_operation, &v);
	if (approval_rc != 0)
		return approval_rc;
	*verdict = v;
	if (v == TOOL_OP_DENY) {
		nb_log_write(tctx ? tctx->logger : NULL, NB_LOG_WARN, "command denied by user: %s",
			     command);
		return NB_ERR_PERMISSION;
	}
	if (v == TOOL_OP_SESSION || v == TOOL_OP_ALWAYS) {
		if (!cmd_ok) {
			int rc = persist_command_programs(tctx, op, v);

			if (rc != 0)
				return rc;
		}
			if (!cwd_ok && cwd && *cwd) {
				nb_buf canonical_cwd NB_CLEANUP(nb_buf_destroy) = {0};
				const char *stored_cwd = cwd;

				if (nb_path_canonical(&canonical_cwd, cwd, NULL) == NB_OK)
					stored_cwd = nb_buf_cstr(&canonical_cwd);
				int rc = v == TOOL_OP_ALWAYS ?
					save_grant(tctx,
						   op->principal ? op->principal : "*",
						   "command_cwd", stored_cwd) :
					tool_context_allow_command_scope(tctx, cwd);
				if (rc < 0)
					return rc;
				else
					nb_log_write(tctx ? tctx->logger : NULL, NB_LOG_INFO,
						     "persisted cwd '%s' %s", stored_cwd,
						     v == TOOL_OP_ALWAYS ? "always"
									 : "for session");
			}
	} else {
		nb_log_write(tctx ? tctx->logger : NULL, NB_LOG_INFO,
			     "command approved once by user");
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
		return NB_ERR_INVAL;
	if (op->kind == TOOL_OP_PATH_DELETE) {
		allow_dirs = tctx->delete_allowed_dirs;
		allow_count = &tctx->delete_allowed_dirs_count;
	} else if (op->kind == TOOL_OP_PATH_WRITE) {
		allow_dirs = tctx->write_allowed_dirs;
		allow_count = &tctx->write_allowed_dirs_count;
	} else {
		allow_dirs = tctx->read_allowed_dirs;
		allow_count = &tctx->read_allowed_dirs_count;
	}
	char canonical[PATH_MAX];
	enum tool_path_op path_op = op->kind == TOOL_OP_PATH_DELETE ? TOOL_PATH_DELETE :
		op->kind == TOOL_OP_PATH_WRITE ? TOOL_PATH_WRITE :
		op->kind == TOOL_OP_PATH_LIST ? TOOL_PATH_LIST : TOOL_PATH_READ;
	int rc = resolve_user_path(tctx, path_op, op->target, canonical, sizeof(canonical));

	if (rc != 0)
		return rc;
	for (int i = 0; i < *allow_count; i++) {
		if (nb_path_is_within(canonical, allow_dirs[i], true))
			return 0;
	}
	if (policy_path_is_allowed(tctx, path_op, canonical))
		return 0;
	struct tool_scoped_grant *grant;

	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&tctx->scoped_grants) &&
	     (grant = (struct tool_scoped_grant *)nb_vec_at(&tctx->scoped_grants, _i_));
	     _i_++) {
		if (grant->kind == (path_op_is_read(path_op) ? TOOL_PATH_READ : path_op) &&
		    strcmp(grant->subject, op->principal ? op->principal : "tool:*") == 0 &&
		    nb_path_is_within(canonical, grant->path, true))
			return 0;
	}
	if (persistent_path_is_allowed(tctx, op->principal ? op->principal : "tool:*",
		path_op_is_read(path_op) ? "read_path" :
		path_op == TOOL_PATH_DELETE ? "delete_path" : "tool_write_path", canonical))
		return 0;
	int approval_rc = tool_context_review(tctx, op, &v);
	if (approval_rc != 0)
		return approval_rc;
	if (v == TOOL_OP_DENY)
		return NB_ERR_PERMISSION;
	if (v == TOOL_OP_SESSION || v == TOOL_OP_ALWAYS) {
		const char *kind = op->kind == TOOL_OP_PATH_WRITE ?
			"tool_write_path" : op->kind == TOOL_OP_PATH_DELETE ?
			"delete_path" : "read_path";
		const char *subject = op->principal ? op->principal : "tool:*";

		/* Grant the exact approved path, never its parent directory. */
		rc = v == TOOL_OP_ALWAYS ? save_grant(tctx, subject, kind, canonical) :
			add_scoped_path_grant(tctx,
				path_op_is_read(path_op) ? TOOL_PATH_READ : path_op,
				subject, canonical, 0);
		if (rc != 0)
			return rc;
	}
	return 0;
}

static int scoped_path_is_allowed(const struct tool_context *tctx, enum tool_path_op operation,
				  const char *principal, const char *path)
{
	struct tool_scoped_grant *grant;
	const char *kind = operation == TOOL_PATH_WRITE ? "write_path" : "delete_path";

	if (policy_path_is_allowed(tctx, operation, path))
		return 1;
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&tctx->scoped_grants) &&
	     (grant = (struct tool_scoped_grant *)nb_vec_at(&tctx->scoped_grants, _i_));
	     _i_++) {
		if (grant->kind == operation && strcmp(grant->subject, principal) == 0 &&
		    nb_path_is_within(path, grant->path, true))
			return 1;
	}
	return persistent_path_is_allowed(tctx, principal, kind, path);
}

static int store_command_path_grant(struct tool_context *tctx, enum tool_path_op operation,
				    const char *principal, const char *path,
				    enum tool_operation_verdict verdict)
{
	if (verdict == TOOL_OP_ALWAYS)
		return save_grant(tctx, principal,
				  operation == TOOL_PATH_WRITE ? "write_path" : "delete_path",
				  path);
	if (verdict == TOOL_OP_SESSION)
		return add_scoped_path_grant(tctx, operation, principal, path, 0);
	return verdict == TOOL_OP_ALLOW ? 0 : NB_ERR_PERMISSION;
}

static int request_command_path(struct tool_context *tctx, enum tool_path_op operation,
				const char *principal, const char *command, const char *path,
				char *resolved, size_t resolved_size)
{
	nb_buf file_value_12 NB_CLEANUP(nb_buf_destroy) = {0};
	int expand_status_12 = NB_OK;

	char local[PATH_MAX];
	char *expanded = NULL;
	struct tool_operation op = {0};
	enum tool_operation_verdict verdict;
	int rc;

	if (!tctx || !principal || !*principal || !path || !*path)
		return NB_ERR_INVAL;
	if (!resolved || !resolved_size) {
		resolved = local;
		resolved_size = sizeof(local);
	}
	if (operation == TOOL_PATH_WRITE) {
		expanded = ((expand_status_12 = nb_path_expand(&file_value_12, path, NULL)) == NB_OK
				    ? strdup(nb_buf_cstr(&file_value_12))
				    : NULL);
		if (!expanded)
			return expand_status_12 != NB_OK ? expand_status_12 : NB_ERR_NOMEM;
		path = expanded;
	}
	rc = permission_path_resolve_absolute(path, resolved, resolved_size);
	free(expanded);
	if (rc != 0)
		return rc;
	if (scoped_path_is_allowed(tctx, operation, principal, resolved))
		return 0;
	if (operation == TOOL_PATH_WRITE && !read_only(tctx) && tctx->output_dir[0] &&
	    nb_path_is_within(resolved, tctx->output_dir, true))
		return 0;
	op.kind = operation == TOOL_PATH_WRITE ? TOOL_OP_PATH_WRITE : TOOL_OP_PATH_DELETE;
	op.tool_name = "exec";
	op.principal = principal;
	op.action = command;
	op.target = resolved;
	op.scope = tctx->grant_project_root;
	rc = tool_context_review(tctx, &op, &verdict);
	if (rc != 0)
		return rc;
	if (operation == TOOL_PATH_WRITE)
		return tool_context_grant_write_access(tctx, principal, resolved, 0, verdict,
						       resolved, resolved_size);
	return store_command_path_grant(tctx, operation, principal, resolved, verdict);
}

static int tool_context_request_write_access_locked(struct tool_context *tctx,
						    const char *principal, const char *command,
						    const char *path, char *resolved,
						    size_t resolved_size)
{
	return request_command_path(tctx, TOOL_PATH_WRITE, principal, command, path, resolved,
				    resolved_size);
}

static int tool_context_grant_write_access_locked(struct tool_context *tctx, const char *principal,
						  const char *path, int create,
						  enum tool_operation_verdict verdict,
						  char *resolved, size_t resolved_size)
{
	char local[PATH_MAX];
	int rc;

	if (!tctx || !principal || !*principal || !path || !*path)
		return NB_ERR_INVAL;
	if (verdict != TOOL_OP_ALLOW && verdict != TOOL_OP_SESSION &&
	    verdict != TOOL_OP_ALWAYS)
		return NB_ERR_PERMISSION;
	if (!resolved || !resolved_size) {
		resolved = local;
		resolved_size = sizeof(local);
	}
	rc = permission_path_resolve_existing_directory(path, create, resolved, resolved_size);
	if (rc != 0)
		return rc;
	return store_command_path_grant(tctx, TOOL_PATH_WRITE, principal, resolved, verdict);
}

static int append_grant_path(nb_vec *paths, const char *path)
{
	for (size_t i = 0; i < paths->len; i++) {
		const char **existing = nb_vec_at(paths, i);

		if (strcmp(*existing, path) == 0)
			return 0;
	}
	const char **slot = nb_vec_push(paths);

	if (!slot)
		return NB_ERR_NOMEM;
	*slot = path;
	return 0;
}

static int tool_context_collect_path_grants_locked(const struct tool_context *tctx,
						   const char *principal, enum tool_path_op op,
						   nb_vec *paths)
{
	struct tool_scoped_grant *grant;
	struct tool_builtin_path *builtin;
	struct permission_grant *persistent;
	const char *kind = op == TOOL_PATH_READ ? "read_path" :
		op == TOOL_PATH_WRITE ? "write_path" : "delete_path";
	int rc;

	if (!tctx || !principal || !paths || paths->elem_size != sizeof(const char *) ||
	    (op != TOOL_PATH_READ && op != TOOL_PATH_WRITE && op != TOOL_PATH_DELETE))
		return NB_ERR_INVAL;
	for (int i = 0; op == TOOL_PATH_READ && i < tctx->read_allowed_dirs_count; i++) {
		rc = append_grant_path(paths, tctx->read_allowed_dirs[i]);
		if (rc != 0)
			return rc;
	}
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&tctx->scoped_grants) &&
	     (grant = (struct tool_scoped_grant *)nb_vec_at(&tctx->scoped_grants, _i_));
	     _i_++) {
		if (grant->kind != op ||
		    (strcmp(grant->subject, principal) != 0 &&
		     strcmp(grant->subject, "*") != 0))
			continue;
		rc = append_grant_path(paths, grant->path);
		if (rc != 0)
			return rc;
	}
	int count = read_only(tctx) && op != TOOL_PATH_READ ? 0 : op == TOOL_PATH_WRITE ?
		tctx->exec_profile_write_dirs_count : tctx->exec_profile_delete_dirs_count;

	for (int i = 0; i < count; i++) {
		const char *path = op == TOOL_PATH_WRITE ? tctx->exec_profile_write_dirs[i] :
			tctx->exec_profile_delete_dirs[i];

		rc = append_grant_path(paths, path);
		if (rc != 0)
			return rc;
	}
	for (int i = 0; op == TOOL_PATH_READ && i < tctx->exec_profile_write_dirs_count; i++) {
		rc = append_grant_path(paths, tctx->exec_profile_write_dirs[i]);
		if (rc != 0)
			return rc;
	}
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&tctx->persistent_grants) &&
	     (persistent = (struct permission_grant *)nb_vec_at(&tctx->persistent_grants, _i_));
	     _i_++) {
		if (strcmp(persistent->resource_kind, grant_kind(tctx, kind)) != 0 ||
		    (strcmp(persistent->subject, principal) != 0 &&
		     strcmp(persistent->subject, "*") != 0))
			continue;
		rc = append_grant_path(paths, persistent->resource);
		if (rc != 0)
			return rc;
	}
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&tctx->builtin_paths) &&
	     (builtin = (struct tool_builtin_path *)nb_vec_at(&tctx->builtin_paths, _i_));
	     _i_++) {
		if ((read_only(tctx) && op != TOOL_PATH_READ) ||
		    !(builtin->operations & (1u << (unsigned int)op)))
			continue;
		rc = append_grant_path(paths, builtin->path);
		if (rc != 0)
			return rc;
	}
	return 0;
}

static int tool_context_add_exec_profile_path_locked(struct tool_context *tctx,
				       enum tool_path_op op,
				       const char *path)
{
	nb_buf file_value_13 NB_CLEANUP(nb_buf_destroy) = {0};
	int expand_status_13 = NB_OK;
	nb_buf file_value_14 NB_CLEANUP(nb_buf_destroy) = {0};
	int expand_status_14 = NB_OK;

	char (*dirs)[TOOL_CONTEXT_ALLOW_PATH_MAX];
	char *expanded;
	char *resolved;
	struct stat st;
	int *count;
	int rc;

	if (!tctx || !path || !*path)
		return NB_ERR_INVAL;
	if (op == TOOL_PATH_WRITE) {
		dirs = tctx->exec_profile_write_dirs;
		count = &tctx->exec_profile_write_dirs_count;
	} else if (op == TOOL_PATH_DELETE) {
		dirs = tctx->exec_profile_delete_dirs;
		count = &tctx->exec_profile_delete_dirs_count;
	} else {
		return NB_ERR_INVAL;
	}
	expanded = ((expand_status_13 = nb_path_expand(&file_value_13, path, NULL)) == NB_OK
			    ? strdup(nb_buf_cstr(&file_value_13))
			    : NULL);
	if (!expanded)
		return expand_status_13 != NB_OK ? expand_status_13 : NB_ERR_NOMEM;
	if (!nb_path_is_absolute(expanded)) {
		free(expanded);
		return NB_ERR_INVAL;
	}
	if (stat(expanded, &st) != 0) {
		rc = nb_status_from_errno(errno, NULL);

		free(expanded);
		return rc;
	}
	if (!S_ISDIR(st.st_mode)) {
		free(expanded);
		return NB_ERR_NOT_DIR;
	}
	resolved =
		((expand_status_14 = nb_path_expand(&file_value_14, expanded, NULL)) == NB_OK &&
				 (expand_status_14 = nb_path_canonical_missing(
					 &file_value_14, nb_buf_cstr(&file_value_14),
						NULL)) == NB_OK
			 ? strdup(nb_buf_cstr(&file_value_14))
			 : NULL);
	free(expanded);
	if (!resolved)
		return expand_status_14 != NB_OK ? expand_status_14 : NB_ERR_NOMEM;
	rc = add_allowed_dir(dirs, count, resolved);
	free(resolved);
	return rc;
}

static int tool_context_request_delete_access_locked(struct tool_context *tctx,
						     const char *principal, const char *command,
						     const char *path, char *resolved,
						     size_t resolved_size)
{
	return request_command_path(tctx, TOOL_PATH_DELETE, principal, command, path, resolved,
				    resolved_size);
}

static int tool_context_grant_delete_access_locked(struct tool_context *tctx, const char *principal,
						   const char *path, char *resolved,
						   size_t resolved_size)
{
	char local[PATH_MAX];
	int rc;

	if (!tctx || !principal || !*principal || !path || !*path)
		return NB_ERR_INVAL;
	if (!resolved || !resolved_size) {
		resolved = local;
		resolved_size = sizeof(local);
	}
	rc = permission_path_resolve_existing_directory(path, 0, resolved, resolved_size);
	if (rc != 0)
		return rc;
	return store_command_path_grant(tctx, TOOL_PATH_DELETE, principal, resolved,
					TOOL_OP_SESSION);
}

static int configured_path_allowed(
	const char *path,
	const char dirs[][TOOL_CONTEXT_ALLOW_PATH_MAX], int count)
{
	for (int i = 0; i < count; i++)
		if (nb_path_is_within(path, dirs[i], true))
			return 1;
	return 0;
}

static const struct {
	unsigned int access;
	enum tool_path_op operation;
	const char *kind;
} directory_access_kinds[] = {
	{TOOL_PATH_ACCESS_READ, TOOL_PATH_READ, "read_path"},
	{TOOL_PATH_ACCESS_WRITE, TOOL_PATH_WRITE, "write_path"},
	{TOOL_PATH_ACCESS_DELETE, TOOL_PATH_DELETE, "delete_path"},
};

static int directory_access_is_allowed(const struct tool_context *tctx,
	const struct tool_directory_access_request *request,
	enum tool_path_op operation, const char *canonical)
{
	int already_allowed =
		(!read_only(tctx) || operation == TOOL_PATH_READ) &&
		((tctx->workdir[0] && nb_path_is_within(canonical, tctx->workdir, true)) ||
		 (tctx->output_dir[0] && nb_path_is_within(canonical, tctx->output_dir, true)) ||
		 permission_temporary_path_is_default(canonical));
	already_allowed = already_allowed ||
		policy_path_is_allowed(tctx, operation, canonical);
	if (operation == TOOL_PATH_READ)
		already_allowed = already_allowed ||
			persistent_path_is_allowed(tctx, request->principal,
				"read_path", canonical);
	else if (operation == TOOL_PATH_WRITE)
		already_allowed = already_allowed ||
				  scoped_path_is_allowed(tctx, TOOL_PATH_WRITE, request->principal,
							 canonical) ||
				  (!read_only(tctx) &&
				   configured_path_allowed(canonical, tctx->exec_profile_write_dirs,
							   tctx->exec_profile_write_dirs_count));
	else
		already_allowed =
			already_allowed ||
			scoped_path_is_allowed(tctx, TOOL_PATH_DELETE, request->principal,
					       canonical) ||
			(!read_only(tctx) &&
			 configured_path_allowed(canonical, tctx->exec_profile_delete_dirs,
						 tctx->exec_profile_delete_dirs_count));
	/* Extending a turn grant to a session requires a new approval. */
	if (request->session_scope && strcmp(request->principal, "*") == 0 &&
	    directory_grant_is_allowed(tctx, operation, canonical, 0) &&
	    !directory_grant_is_allowed(tctx, operation, canonical, 1))
		already_allowed = 0;
	return already_allowed;
}

static int store_directory_access(struct tool_context *tctx,
	const struct tool_directory_access_request *request, const char *canonical,
	unsigned int access, enum tool_operation_verdict verdict)
{
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	int persistent = verdict == TOOL_OP_ALWAYS;
	nb_vec *storage = persistent ? &tctx->persistent_grants : &tctx->scoped_grants;
	size_t before = storage->len;

	if (before > SIZE_MAX - 3)
		return NB_ERR_RANGE;
	int rc = (nb_vec_reserve(storage, before + 3) ? 0 : NB_ERR_NOMEM);
	int transaction = persistent && tctx->grant_db && tctx->grant_project_root[0];

	if (rc != 0)
		return rc;
	if (transaction) {
		rc = db_lock_acquire(tctx->grant_db, &lock);
		if (rc == 0)
			rc = db_exec(tctx->grant_db, "SAVEPOINT directory_access;");
		if (rc != 0)
			return rc;
	}
	for (size_t i = 0; i < sizeof(directory_access_kinds) / sizeof(directory_access_kinds[0]);
	     i++) {
		if (!(access & directory_access_kinds[i].access))
			continue;
		if (persistent)
			rc = save_grant(tctx, request->principal, directory_access_kinds[i].kind,
				canonical);
		else
			rc = add_scoped_path_grant(tctx, directory_access_kinds[i].operation,
				request->principal, canonical,
				!request->session_scope && verdict != TOOL_OP_SESSION);
		if (rc != 0)
			break;
	}
	if (transaction && rc == 0)
		rc = db_exec(tctx->grant_db, "RELEASE directory_access;");
	if (rc != 0) {
		if (transaction) {
			(void)db_exec(tctx->grant_db, "ROLLBACK TO directory_access;");
			(void)db_exec(tctx->grant_db, "RELEASE directory_access;");
		}
		storage->len = before;
	}
	return rc;
}

static char *directory_access_details(const struct tool_operation *operation)
{
	cJSON *details = cJSON_CreateObject();
	char *encoded = NULL;

	if (details && cJSON_AddStringToObject(details, "access",
	    tool_path_access_name(operation->path_access)) &&
	    cJSON_AddStringToObject(details, "target", operation->target) &&
	    cJSON_AddStringToObject(details, "scope", operation->scope) &&
	    cJSON_AddStringToObject(details, "reason", operation->reason ? operation->reason : ""))
		encoded = cJSON_PrintUnformatted(details);
	cJSON_Delete(details);
	return encoded;
}

static int tool_context_request_directory_access_locked(struct tool_context *tctx,
	const struct tool_directory_access_request *request,
	char *resolved, size_t resolved_size)
{
	char canonical[PATH_MAX];
	char local[PATH_MAX];
	unsigned int pending = 0;
	int rc;

	if (!tctx || !request || !request->principal || !*request->principal ||
	    strlen(request->principal) >= TOOL_GRANT_SUBJECT_MAX ||
	    !request->command || !*request->command)
		return NB_ERR_INVAL;
	rc = tool_context_resolve_directory_access(request->path, request->access,
		canonical, sizeof(canonical));
	if (rc != 0)
		return rc;
	if (!resolved || !resolved_size) {
		resolved = local;
		resolved_size = sizeof(local);
	}
	if (strlen(canonical) >= resolved_size)
		return NB_ERR_NAME_TOO_LONG;
	memcpy(resolved, canonical, strlen(canonical) + 1);
	for (size_t i = 0; i < sizeof(directory_access_kinds) / sizeof(directory_access_kinds[0]);
	     i++) {
		if ((request->access & directory_access_kinds[i].access) &&
		    !directory_access_is_allowed(tctx, request,
			directory_access_kinds[i].operation, canonical))
			pending |= directory_access_kinds[i].access;
	}
	if (!pending)
		return 0;
	struct tool_operation operation = {0};

	operation.kind = (pending & (pending - 1u)) ? TOOL_OP_DIRECTORY_ACCESS :
		pending & TOOL_PATH_ACCESS_DELETE ? TOOL_OP_PATH_DELETE :
		pending & TOOL_PATH_ACCESS_WRITE ? TOOL_OP_PATH_WRITE : TOOL_OP_PATH_READ;
	operation.tool_name = "request_permissions";
	operation.principal = request->principal;
	operation.action = request->command;
	operation.target = canonical;
	operation.scope = request->session_scope ? "session" : "turn";
	operation.reason = request->reason;
	operation.path_access = pending;
	operation.require_user = request->session_scope &&
		tctx->permissions.policy.reviewer == PERMISSION_MODEL;
	char *details = directory_access_details(&operation);

	if (!details)
		return NB_ERR_NOMEM;
	operation.details_json = details;
	enum tool_operation_verdict verdict;
	int approval_rc = tool_context_review(tctx, &operation, &verdict);

	free(details);
	if (approval_rc != 0)
		return approval_rc;
	if (verdict == TOOL_OP_DENY)
		return NB_ERR_PERMISSION;
	if (verdict != TOOL_OP_ALLOW && verdict != TOOL_OP_SESSION && verdict != TOOL_OP_ALWAYS)
		return NB_ERR_INVAL;
	struct stat st;

	if (stat(canonical, &st) != 0) {
		if (errno != ENOENT || !(request->access & TOOL_PATH_ACCESS_WRITE))
			return nb_status_from_errno(errno, NULL);
		rc = nb_file_mkdir(canonical, true, NULL);
		if (rc != 0)
			return rc;
	}
	char checked[PATH_MAX];

	rc = tool_context_resolve_directory_access(canonical, request->access,
		checked, sizeof(checked));
	if (rc != 0)
		return rc;
	if (strcmp(checked, canonical) != 0)
		return NB_ERR_STALE;
	return store_directory_access(tctx, request, canonical, pending, verdict);
}

static int tool_context_request_scoped_access_locked(struct tool_context *tctx,
				       enum tool_path_op operation,
				       const char *principal,
				       const char *command, const char *path,
				       int session_scope,
				       char *resolved, size_t resolved_size)
{
	unsigned int access = operation == TOOL_PATH_READ ? TOOL_PATH_ACCESS_READ :
		operation == TOOL_PATH_WRITE ? TOOL_PATH_ACCESS_WRITE :
		operation == TOOL_PATH_DELETE ? TOOL_PATH_ACCESS_DELETE : 0;
	struct tool_directory_access_request request = {
		.access = access, .principal = principal, .command = command,
		.path = path, .session_scope = session_scope,
	};

	return tool_context_request_directory_access(tctx, &request, resolved, resolved_size);
}

static void tool_context_clear_turn_grants_locked(struct tool_context *tctx)
{
	struct tool_scoped_grant *grants;
	size_t dst = 0;

	if (!tctx || !((void *)tctx->scoped_grants.data))
		return;
	grants = ((void *)tctx->scoped_grants.data);
	for (size_t src = 0; src < tctx->scoped_grants.len; src++) {
		if (grants[src].turn_only)
			continue;
		if (dst != src)
			grants[dst] = grants[src];
		dst++;
	}
	tctx->scoped_grants.len = dst;
}

static int tool_context_check_operation_locked(struct tool_context *tctx,
				 const struct tool_operation *op)
{
	enum tool_operation_verdict verdict;

	return tool_context_check_operation_verdict(tctx, op, &verdict);
}

static int check_tool_call(struct tool_context *tctx, const struct tool_operation *op,
	enum tool_operation_verdict *verdict)
{
	const char *principal = op->principal ? op->principal : op->tool_name;
	struct permission_grant *grant;

	if (!principal || !*principal || strlen(principal) >= TOOL_CONTEXT_ACTION_MAX)
		return NB_ERR_INVAL;
	for (int i = 0; i < tctx->tool_calls_count; i++) {
		const struct tool_call_grant *cached = &tctx->tool_calls[i];

		if (!strcmp(cached->principal, principal) && cached->host == !!op->host &&
		    cached->network == !!op->network) {
			*verdict = TOOL_OP_ALLOW;
			return 0;
		}
	}
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&tctx->persistent_grants) &&
	     (grant = (struct permission_grant *)nb_vec_at(&tctx->persistent_grants, _i_));
	     _i_++) {
		if (!op->host && !op->network &&
		    !strcmp(grant->resource_kind, grant_kind(tctx, "tool_call")) &&
		    !strcmp(grant->subject, principal)) {
			*verdict = TOOL_OP_ALLOW;
			return 0;
		}
	}
	int rc = tool_context_review(tctx, op, verdict);

	if (rc == 0 && *verdict == TOOL_OP_ALWAYS && !op->host && !op->network)
		return save_grant(tctx, principal, "tool_call", principal);
	if (rc == 0 && (*verdict == TOOL_OP_SESSION || *verdict == TOOL_OP_ALWAYS)) {
		if (tctx->tool_calls_count >= TOOL_CONTEXT_ALLOW_MAX)
			return NB_ERR_NO_SPACE;
		struct tool_call_grant *cached = &tctx->tool_calls[tctx->tool_calls_count++];

		snprintf(cached->principal, sizeof(cached->principal), "%s", principal);
		cached->host = !!op->host;
		cached->network = !!op->network;
	}
	return rc;
}

static int tool_context_check_operation_verdict_locked(
	struct tool_context *tctx, const struct tool_operation *op,
	enum tool_operation_verdict *verdict)
{
	if (!tctx || !op)
		return NB_ERR_INVAL;
	if (!verdict)
		return NB_ERR_INVAL;
	*verdict = TOOL_OP_DENY;
	if (op->forbidden || op->require_user || op->require_review)
		return tool_context_review(tctx, op, verdict);
	switch (op->kind) {
	case TOOL_OP_COMMAND:
		return check_command_operation(tctx, op, verdict);
	case TOOL_OP_PATH_READ:
	case TOOL_OP_PATH_LIST:
	case TOOL_OP_PATH_WRITE:
	case TOOL_OP_PATH_DELETE:
	{
		int rc = check_path_operation(tctx, op);

		if (rc == 0)
			*verdict = TOOL_OP_ALLOW;
		return rc;
	}
	case TOOL_OP_DIRECTORY_ACCESS:
	{
		struct tool_directory_access_request request = {
			.access = op->path_access,
			.principal = op->principal ? op->principal : "*",
			.command = op->action ? op->action : "Access the requested directory",
			.path = op->target, .reason = op->reason,
			.session_scope = op->scope && strcmp(op->scope, "session") == 0,
		};
		int rc = tool_context_request_directory_access(tctx, &request, NULL, 0);

		if (rc == 0)
			*verdict = TOOL_OP_ALLOW;
		return rc;
	}
	case TOOL_OP_NETWORK:
	case TOOL_OP_EXTERNAL_SEND:
		return tool_context_review(tctx, op, verdict);
	case TOOL_OP_TOOL_CALL:
		return check_tool_call(tctx, op, verdict);
	}
	return NB_ERR_INVAL;
}

int tool_context_review(struct tool_context *tctx,
	const struct tool_operation *op, enum tool_operation_verdict *verdict)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_review_locked(tctx, op, verdict);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

int tool_context_set_permission_policy(struct tool_context *tctx,
	const struct permission_policy *policy)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_set_permission_policy_locked(tctx, policy);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

int tool_context_get_permission_policy(const struct tool_context *tctx,
				       struct permission_policy *policy)
{
	if (!tctx || !policy)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	*policy = tctx->permissions.policy;
	nb_mutex_unlock(tctx->authority_lock);
	return 0;
}

void tool_context_operation_approval_borrow(struct tool_context *tctx,
					    tool_operation_approval_fn *callback, void **user_data)
{
	if (callback)
		*callback = NULL;
	if (user_data)
		*user_data = NULL;
	if (!tctx)
		return;
	nb_mutex_lock(tctx->authority_lock);
	if (callback)
		*callback = tctx->operation_approval_fn;
	if (user_data)
		*user_data = tctx->operation_approval_user_data;
	nb_mutex_unlock(tctx->authority_lock);
}

void tool_context_set_review_adapters(struct tool_context *tctx, permission_review_fn user_review,
				      void *user_data, permission_review_fn model_review,
				      void *model_data)
{
	if (!tctx)
		return;
	nb_mutex_lock(tctx->authority_lock);
	tctx->permissions.user_review = user_review;
	tctx->permissions.user_data = user_data;
	tctx->permissions.model_review = model_review;
	tctx->permissions.model_data = model_data;
	nb_mutex_unlock(tctx->authority_lock);
}

void tool_context_reset_denials(struct tool_context *tctx)
{
	if (!tctx)
		return;
	nb_mutex_lock(tctx->authority_lock);
	tctx->permissions.consecutive_denials = 0;
	nb_mutex_unlock(tctx->authority_lock);
}

struct permission_context *tool_context_permission_context_borrow(struct tool_context *tctx)
{
	return tctx ? &tctx->permissions : NULL;
}

void tool_context_set_default_timeout(struct tool_context *tctx, int seconds)
{
	if (!tctx)
		return;
	nb_mutex_lock(tctx->authority_lock);
	tool_context_set_default_timeout_locked(tctx, seconds);
	nb_mutex_unlock(tctx->authority_lock);
	return;
}

int tool_context_default_timeout(const struct tool_context *tctx)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_default_timeout_locked(tctx);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

void tool_context_add_read_allowed_dir(struct tool_context *tctx,
				       const char *dir)
{
	if (!tctx)
		return;
	nb_mutex_lock(tctx->authority_lock);
	tool_context_add_read_allowed_dir_locked(tctx, dir);
	nb_mutex_unlock(tctx->authority_lock);
	return;
}

void tool_context_add_write_allowed_dir(struct tool_context *tctx,
					const char *dir)
{
	if (!tctx)
		return;
	nb_mutex_lock(tctx->authority_lock);
	tool_context_add_write_allowed_dir_locked(tctx, dir);
	nb_mutex_unlock(tctx->authority_lock);
	return;
}

void tool_context_set_operation_approval(struct tool_context *tctx,
					 tool_operation_approval_fn fn,
					 void *user_data)
{
	if (!tctx)
		return;
	nb_mutex_lock(tctx->authority_lock);
	tool_context_set_operation_approval_locked(tctx, fn, user_data);
	nb_mutex_unlock(tctx->authority_lock);
	return;
}

int tool_context_reload_persistent_grants(struct tool_context *tctx)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_reload_persistent_grants_locked(tctx);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

int tool_context_set_grant_store(struct tool_context *tctx, struct db *db,
				 const char *project_root)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_set_grant_store_locked(tctx, db, project_root);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

int tool_context_add_builtin_path(struct tool_context *tctx,
				 enum tool_path_op op, const char *path)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_add_builtin_path_locked(tctx, op, path);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

int tool_context_authorize_path(struct tool_context *tctx,
				enum tool_path_op op, const char *path,
				char *resolved, size_t resolved_size)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_authorize_path_locked(tctx, op, path, resolved, resolved_size);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

int tool_context_allow_command_pattern(struct tool_context *tctx,
				       const char *pattern)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_allow_command_pattern_locked(tctx, pattern);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

int tool_context_allow_command_scope(struct tool_context *tctx, const char *path)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_allow_command_scope_locked(tctx, path);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

int tool_context_request_write_access(struct tool_context *tctx,
				      const char *principal,
				      const char *command,
				      const char *path,
				      char *resolved,
				      size_t resolved_size)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_request_write_access_locked(tctx, principal,
		command, path, resolved, resolved_size);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

int tool_context_grant_write_access(
	struct tool_context *tctx, const char *principal, const char *path,
	int create, enum tool_operation_verdict verdict,
	char *resolved, size_t resolved_size)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_grant_write_access_locked(tctx, principal, path,
		create, verdict, resolved, resolved_size);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

int tool_context_add_exec_profile_path(struct tool_context *tctx,
				       enum tool_path_op op,
				       const char *path)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_add_exec_profile_path_locked(tctx, op, path);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

int tool_context_request_delete_access(struct tool_context *tctx,
				       const char *principal,
				       const char *command,
				       const char *path,
				       char *resolved,
				       size_t resolved_size)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_request_delete_access_locked(tctx, principal,
		command, path, resolved, resolved_size);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

int tool_context_grant_delete_access(struct tool_context *tctx,
				     const char *principal, const char *path,
				     char *resolved, size_t resolved_size)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_grant_delete_access_locked(tctx, principal, path,
		resolved, resolved_size);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

int tool_context_request_directory_access(struct tool_context *tctx,
	const struct tool_directory_access_request *request,
	char *resolved, size_t resolved_size)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_request_directory_access_locked(tctx, request,
		resolved, resolved_size);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

int tool_context_request_scoped_access(struct tool_context *tctx,
				       enum tool_path_op operation,
				       const char *principal,
				       const char *command, const char *path,
				       int session_scope,
				       char *resolved, size_t resolved_size)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_request_scoped_access_locked(tctx, operation,
		principal, command, path, session_scope, resolved, resolved_size);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

void tool_context_clear_turn_grants(struct tool_context *tctx)
{
	if (!tctx)
		return;
	nb_mutex_lock(tctx->authority_lock);
	tool_context_clear_turn_grants_locked(tctx);
	nb_mutex_unlock(tctx->authority_lock);
	return;
}

int tool_context_check_operation(struct tool_context *tctx,
				 const struct tool_operation *op)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_check_operation_locked(tctx, op);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

int tool_context_check_operation_verdict(
	struct tool_context *tctx, const struct tool_operation *op,
	enum tool_operation_verdict *verdict)
{
	if (!tctx)
		return NB_ERR_INVAL;
	nb_mutex_lock(tctx->authority_lock);
	int rc = tool_context_check_operation_verdict_locked(tctx, op, verdict);
	nb_mutex_unlock(tctx->authority_lock);
	return rc;
}

const char *tool_context_grant_project_root_borrow(const struct tool_context *tctx)
{
	return tctx ? tctx->grant_project_root : NULL;
}

struct nb_log *tool_context_logger_borrow(const struct tool_context *tctx)
{
	return tctx ? tctx->logger : NULL;
}

static void tool_path_snapshot_cleanup(struct tool_path_snapshot *snapshot)
{
	for (int i = 0; i < snapshot->count; i++)
		free(snapshot->paths[i]);
	free(snapshot->paths);
	memset(snapshot, 0, sizeof(*snapshot));
}

void tool_permission_snapshot_cleanup(struct tool_permission_snapshot *snapshot)
{
	if (!snapshot)
		return;
	free(snapshot->workdir);
	free(snapshot->output_dir);
	tool_path_snapshot_cleanup(&snapshot->read);
	tool_path_snapshot_cleanup(&snapshot->write);
	tool_path_snapshot_cleanup(&snapshot->remove);
	memset(snapshot, 0, sizeof(*snapshot));
}

static int snapshot_path_grants(const struct tool_context *tctx, const char *principal,
				enum tool_path_op operation, struct tool_path_snapshot *snapshot)
{
	nb_vec borrowed NB_CLEANUP(nb_vec_cleanup) = {0};
	int rc = (nb_vec_init(&borrowed, sizeof(*snapshot->paths)),
		  nb_vec_reserve(&borrowed, 16) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = tool_context_collect_path_grants_locked(tctx, principal, operation, &borrowed);
	if (rc != 0)
		return rc;
	if (borrowed.len > INT_MAX || borrowed.len > SIZE_MAX / sizeof(*snapshot->paths))
		return NB_ERR_RANGE;
	if (!borrowed.len)
		return 0;
	snapshot->paths = calloc(borrowed.len, sizeof(*snapshot->paths));
	if (!snapshot->paths)
		return NB_ERR_NOMEM;
	snapshot->count = (int)borrowed.len;
	for (size_t i = 0; i < borrowed.len; i++) {
		const char **path = nb_vec_at(&borrowed, i);
		snapshot->paths[i] = strdup(*path);
		if (!snapshot->paths[i])
			return NB_ERR_NOMEM;
	}
	return 0;
}

int tool_context_permission_snapshot_create(const struct tool_context *tctx, const char *principal,
					    struct tool_permission_snapshot *snapshot)
{
	if (!tctx || !principal || !snapshot)
		return NB_ERR_INVAL;
	if (snapshot->workdir || snapshot->output_dir || snapshot->read.paths ||
	    snapshot->write.paths || snapshot->remove.paths)
		return NB_ERR_EXISTS;
	struct tool_permission_snapshot owned NB_CLEANUP(tool_permission_snapshot_cleanup) = {0};
	nb_mutex_lock(tctx->authority_lock);
	owned.policy = tctx->permissions.policy;
	owned.logger = tctx->logger;
	owned.workdir = strdup(tctx->workdir);
	owned.output_dir = strdup(tctx->output_dir);
	int rc = owned.workdir && owned.output_dir ? 0 : NB_ERR_NOMEM;
	if (rc == 0)
		rc = snapshot_path_grants(tctx, principal, TOOL_PATH_READ, &owned.read);
	if (rc == 0)
		rc = snapshot_path_grants(tctx, principal, TOOL_PATH_WRITE, &owned.write);
	if (rc == 0)
		rc = snapshot_path_grants(tctx, principal, TOOL_PATH_DELETE, &owned.remove);
	nb_mutex_unlock(tctx->authority_lock);
	if (rc == 0) {
		*snapshot = owned;
		memset(&owned, 0, sizeof(owned));
	}
	return rc;
}
