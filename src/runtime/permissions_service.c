#include "runtime/runtime.h"

#include "db/permission_grant.h"
#include "runtime/runtime_internal.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "config/edit.h"
#include "agent/tools/exec_jobs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

int runtime_permission_preset(const char *name, struct permission_policy *out)
{
	if (!name || !out)
		return NB_ERR_INVAL;
	*out = permission_policy_default();
	if (!strcmp(name, "ask"))
		return 0;
	if (!strcmp(name, "auto"))
		out->reviewer = PERMISSION_MODEL;
	else if (!strcmp(name, "full")) {
		out->access = PERMISSION_FULL;
		out->approval = PERMISSION_NEVER;
	} else if (!strcmp(name, "read_only"))
		out->access = PERMISSION_READ_ONLY;
	else
		return NB_ERR_INVAL;
	return 0;
}

const char *runtime_permission_preset_name(const struct permission_policy *policy)
{
	static const char *const names[] = {"ask", "auto", "full", "read_only"};

	if (permission_policy_validate(policy) != 0)
		return "invalid";
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		struct permission_policy candidate;

		(void)runtime_permission_preset(names[i], &candidate);
		if (candidate.access == policy->access && candidate.approval == policy->approval &&
		    candidate.reviewer == policy->reviewer)
			return names[i];
	}
	return "custom";
}

int runtime_permission_get_policy(struct runtime *runtime, struct permission_policy *out)
{
	if (!runtime || !out)
		return NB_ERR_INVAL;
	if (nb_mutex_trylock(runtime->context.execution_lock) != 0)
		return NB_ERR_BUSY;
	int rc = tool_context_get_permission_policy(runtime->context.tctx, out);
	nb_mutex_unlock(runtime->context.execution_lock);
	return rc;
}

static int permission_children_active(struct runtime *runtime)
{
	struct agent_info *tasks = NULL;
	int count = 0;
	int active = exec_jobs_active(runtime->jobs);

	if (runtime->context.agents) {
		int rc = agent_control_list_copy(runtime->context.agents, 0, &tasks,
							  &count);

		if (rc != 0)
			return rc;
		for (int i = 0; i < count; i++) {
			if (tasks[i].status == AGENT_PENDING ||
			    tasks[i].status == AGENT_RUNNING)
				active = 1;
		}
		agent_control_free_list(tasks, count);
	}
	return active;
}

static int permission_save_policy(struct runtime *runtime, const struct permission_policy *policy)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_1 NB_CLEANUP(nb_charp_cleanup) = NULL;

	nb_buf patch NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf lock_path NB_CLEANUP(nb_buf_cleanup) = {0};
	char *before = NULL;
	char *after = NULL;
	struct config_validation_error validation = {0};
	const char *path = runtime->context.config_path;
	int fd = -1;
	int rc;

	if (!*path)
		return NB_ERR_NOT_FOUND;
	rc = (nb_buf_init(&lock_path),
	      nb_buf_reserve(&lock_path, strlen(path) + 6) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_printf(&lock_path, "%s.lock", path) ? 0 : NB_ERR_NOMEM);
	if (rc == 0) {
		fd = open(nb_buf_cstr(&lock_path), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		if (fd < 0)
			rc = nb_status_from_errno(errno, NULL);
		else if (flock(fd, LOCK_EX | LOCK_NB) != 0)
			rc = nb_status_from_errno(errno, NULL);
	}
	if (rc == 0) {
		before = (nb_file_read_all(path, &file_value_1, 0, NULL) == NB_OK &&
					  (file_copy_1 = (char *)malloc(nb_buf_len(&file_value_1) +
									1)) != NULL
				  ? (memcpy(file_copy_1, nb_buf_cstr(&file_value_1),
					    nb_buf_len(&file_value_1) + 1),
				     nb_charp_steal(&file_copy_1))
				  : NULL);
		if (!before)
			rc = errno ? nb_status_from_errno(errno, NULL) : NB_ERR_IO;
	}
	if (rc == 0)
		rc = (nb_buf_init(&patch), nb_buf_reserve(&patch, 128) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_printf(&patch,
				    "[permissions]\naccess = \"%s\"\napproval = \"%s\"\nreviewer = "
				    "\"%s\"\n",
				    permission_access_name(policy->access),
				    permission_approval_name(policy->approval),
				    permission_reviewer_name(policy->reviewer))
			      ? 0
			      : NB_ERR_NOMEM);
	if (rc == 0)
		rc = config_text_merge_copy(before, nb_buf_cstr(&patch), &after);
	if (rc == 0)
		rc = config_validate_text(after, &validation);
	if (rc == 0)
		rc = nb_file_write_all_atomic(
			path, after, strlen(after),
			&(nb_file_write_options){.new_file_mode = 0600, .sync_dir = true}, NULL);
	if (fd >= 0)
		close(fd);
	free(after);
	free(before);
	nb_buf_cleanup(&patch);
	nb_buf_cleanup(&lock_path);
	return rc;
}

int runtime_permission_set_policy(struct runtime *runtime, const struct permission_policy *policy)
{
	int rc;

	if (!runtime || permission_policy_validate(policy) != 0)
		return NB_ERR_INVAL;
	if (nb_mutex_trylock(runtime->context.execution_lock) != 0)
		return NB_ERR_BUSY;
	rc = permission_children_active(runtime);
	if (rc > 0)
		rc = NB_ERR_BUSY;
	else if (rc == 0)
		rc = permission_save_policy(runtime, policy);
	if (rc == 0) {
		rc = tool_context_set_permission_policy(runtime->context.tctx, policy);
		runtime->context.config.permissions = *policy;
		runtime->context.config.permissions_explicit = 1;
	}
	nb_mutex_unlock(runtime->context.execution_lock);
	return rc;
}

struct permission_list_state {
	nb_vec grants;
};

static int collect_permission(const struct permission_grant *grant, void *user_data)
{
	struct permission_list_state *state = user_data;
	struct runtime_permission_grant *item;

	item = nb_vec_push(&state->grants);
	if (!item)
		return NB_ERR_NOMEM;
	memset(item, 0, sizeof(*item));
	item->id = grant->id;
	item->created_at = grant->created_at;
	snprintf(item->subject, sizeof(item->subject), "%s", grant->subject);
	snprintf(item->resource_kind, sizeof(item->resource_kind), "%s", grant->resource_kind);
	snprintf(item->resource, sizeof(item->resource), "%s", grant->resource);
	snprintf(item->project_root, sizeof(item->project_root), "%s", grant->project_root);
	return 0;
}

int runtime_permission_list(struct runtime *runtime, struct runtime_permission_grant **out,
			    int *count)
{
	struct permission_list_state state = {0};
	struct tool_context *tctx;
	int rc;

	if (!runtime || !out || !count)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	*count = 0;
	tctx = runtime->context.tctx;
	if (!tctx || !tool_context_grant_project_root_borrow(tctx)[0])
		return NB_ERR_NOT_FOUND;
	rc = (nb_vec_init(&state.grants, sizeof(struct runtime_permission_grant)),
	      nb_vec_reserve(&state.grants, 8) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	nb_mutex_lock(runtime->context.execution_lock);
	rc = permission_grant_each(&runtime->context.database,
				   tool_context_grant_project_root_borrow(tctx), collect_permission,
				   &state);
	nb_mutex_unlock(runtime->context.execution_lock);
	if (rc == 0 && state.grants.len > INT_MAX)
		rc = NB_ERR_RANGE;
	if (rc != 0) {
		nb_vec_cleanup(&state.grants);
		return rc;
	}
	*out = state.grants.len ? malloc(state.grants.len * state.grants.elem_size) : NULL;
	if (state.grants.len && !*out) {
		nb_vec_cleanup(&state.grants);
		return NB_ERR_NOMEM;
	}
	if (state.grants.len)
		memcpy(*out, state.grants.data, state.grants.len * state.grants.elem_size);
	*count = (int)state.grants.len;
	state.grants.len = 0;
	nb_vec_cleanup(&state.grants);
	nb_vec_cleanup(&state.grants);
	return 0;
}

void runtime_permission_list_free(struct runtime_permission_grant *grants)
{
	free(grants);
}

static int reload_after_delete(struct runtime *runtime, int rc, int deleted)
{
	if (rc == 0 && deleted > 0)
		rc = tool_context_reload_persistent_grants(runtime->context.tctx);
	return rc;
}

int runtime_permission_revoke_id(struct runtime *runtime, int64_t id, int *deleted)
{
	struct tool_context *tctx;
	int rc;

	if (!runtime || !deleted || id <= 0)
		return NB_ERR_INVAL;
	tctx = runtime->context.tctx;
	if (!tctx || !tool_context_grant_project_root_borrow(tctx)[0])
		return NB_ERR_NOT_FOUND;
	nb_mutex_lock(runtime->context.execution_lock);
	rc = permission_grant_delete_id(&runtime->context.database,
					tool_context_grant_project_root_borrow(tctx), id, deleted);
	rc = reload_after_delete(runtime, rc, *deleted);
	nb_mutex_unlock(runtime->context.execution_lock);
	return rc;
}

int runtime_permission_revoke_subject(struct runtime *runtime, const char *subject, int *deleted)
{
	struct tool_context *tctx;
	int rc;

	if (!runtime || !subject || !*subject || !deleted)
		return NB_ERR_INVAL;
	tctx = runtime->context.tctx;
	if (!tctx || !tool_context_grant_project_root_borrow(tctx)[0])
		return NB_ERR_NOT_FOUND;
	nb_mutex_lock(runtime->context.execution_lock);
	rc = permission_grant_delete_subject(&runtime->context.database,
					     tool_context_grant_project_root_borrow(tctx), subject,
					     deleted);
	rc = reload_after_delete(runtime, rc, *deleted);
	nb_mutex_unlock(runtime->context.execution_lock);
	return rc;
}

int runtime_permission_clear(struct runtime *runtime, int all_projects, int *deleted)
{
	struct tool_context *tctx;
	int rc;

	if (!runtime || !deleted)
		return NB_ERR_INVAL;
	tctx = runtime->context.tctx;
	if (!tctx || !tool_context_grant_project_root_borrow(tctx)[0])
		return NB_ERR_NOT_FOUND;
	nb_mutex_lock(runtime->context.execution_lock);
	if (all_projects)
		rc = permission_grant_clear_all(&runtime->context.database, deleted);
	else
		rc = permission_grant_clear_project(&runtime->context.database,
						    tool_context_grant_project_root_borrow(tctx),
						    deleted);
	rc = reload_after_delete(runtime, rc, *deleted);
	nb_mutex_unlock(runtime->context.execution_lock);
	return rc;
}
