#include "runtime/runtime.h"

#include "db/permission_grant.h"
#include "runtime/runtime_internal.h"
#include "util/array.h"
#include "util/error.h"
#include "util/file.h"
#include "util/buf.h"
#include "config/edit.h"
#include "agent/tools/exec_jobs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

int runtime_permission_preset(const char *name, struct permission_policy *out)
{
	if (!name || !out)
		return -EINVAL;
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
		return -EINVAL;
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
		return -EINVAL;
	if (pthread_mutex_trylock(&runtime->context.execution_lock) != 0)
		return -EBUSY;
	int rc = tool_context_get_permission_policy(runtime->context.tctx, out);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return rc;
}

static int permission_children_active(struct runtime *runtime)
{
	struct sub_agent_task_info *tasks = NULL;
	int count = 0;
	int active = exec_jobs_active(runtime->jobs);

	if (runtime->context.sub_agents) {
		int rc = sub_agent_runtime_list_tasks(runtime->context.sub_agents, 0, &tasks,
						      &count);

		if (rc != 0)
			return rc;
		for (int i = 0; i < count; i++) {
			if (tasks[i].status == SUB_AGENT_PENDING ||
			    tasks[i].status == SUB_AGENT_RUNNING)
				active = 1;
		}
		sub_agent_runtime_free_task_list(tasks, count);
	}
	return active;
}

static int permission_save_policy(struct runtime *runtime, const struct permission_policy *policy)
{
	morph_buf_t patch = {0};
	morph_buf_t lock_path = {0};
	char *before = NULL;
	char *after = NULL;
	struct config_validation_error validation = {0};
	const char *path = runtime->context.config_path;
	int fd = -1;
	int rc;

	if (!*path)
		return -ENOENT;
	rc = morph_buf_init(&lock_path, strlen(path) + 6);
	if (rc == 0)
		rc = morph_buf_printf(&lock_path, "%s.lock", path);
	if (rc == 0) {
		fd = open(morph_buf_cstr(&lock_path), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		if (fd < 0)
			rc = -errno;
		else if (flock(fd, LOCK_EX | LOCK_NB) != 0)
			rc = -errno;
	}
	if (rc == 0) {
		before = file_read_all(path, NULL);
		if (!before)
			rc = errno ? -errno : -EIO;
	}
	if (rc == 0)
		rc = morph_buf_init(&patch, 128);
	if (rc == 0)
		rc = morph_buf_printf(
			&patch,
			"[permissions]\naccess = \"%s\"\napproval = \"%s\"\nreviewer = \"%s\"\n",
			permission_access_name(policy->access),
			permission_approval_name(policy->approval),
			permission_reviewer_name(policy->reviewer));
	if (rc == 0)
		rc = config_text_merge(before, morph_buf_cstr(&patch), &after);
	if (rc == 0)
		rc = config_validate_text(after, &validation);
	if (rc == 0)
		rc = file_write_atomic(path, after, strlen(after));
	if (fd >= 0)
		close(fd);
	free(after);
	free(before);
	morph_buf_cleanup(&patch);
	morph_buf_cleanup(&lock_path);
	return rc;
}

int runtime_permission_set_policy(struct runtime *runtime, const struct permission_policy *policy)
{
	int rc;

	if (!runtime || permission_policy_validate(policy) != 0)
		return -EINVAL;
	if (pthread_mutex_trylock(&runtime->context.execution_lock) != 0)
		return -EBUSY;
	rc = permission_children_active(runtime);
	if (rc > 0)
		rc = -EBUSY;
	else if (rc == 0)
		rc = permission_save_policy(runtime, policy);
	if (rc == 0) {
		rc = tool_context_set_permission_policy(runtime->context.tctx, policy);
		runtime->context.config.permissions = *policy;
		runtime->context.config.permissions_explicit = 1;
	}
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return rc;
}

struct permission_list_state {
	morph_array_t grants;
};

static int collect_permission(const struct permission_grant *grant, void *user_data)
{
	struct permission_list_state *state = user_data;
	struct runtime_permission_grant *item;

	item = morph_array_push(&state->grants);
	if (!item)
		return -ENOMEM;
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
	struct permission_list_state state;
	struct tool_context *tctx;
	int rc;

	if (!runtime || !out || !count)
		return -EINVAL;
	*out = NULL;
	*count = 0;
	tctx = runtime->context.tctx;
	if (!tctx || !tool_context_grant_project_root(tctx)[0])
		return -ENOENT;
	rc = morph_array_init(&state.grants, 8, sizeof(struct runtime_permission_grant));
	if (rc != 0)
		return rc;
	pthread_mutex_lock(&runtime->context.execution_lock);
	rc = permission_grant_each(&runtime->context.database,
				   tool_context_grant_project_root(tctx), collect_permission,
				   &state);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	if (rc != 0) {
		morph_array_cleanup(&state.grants);
		return rc;
	}
	*out = state.grants.elts;
	*count = (int)state.grants.nelts;
	state.grants.elts = NULL;
	morph_array_cleanup(&state.grants);
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
		return -EINVAL;
	tctx = runtime->context.tctx;
	if (!tctx || !tool_context_grant_project_root(tctx)[0])
		return -ENOENT;
	pthread_mutex_lock(&runtime->context.execution_lock);
	rc = permission_grant_delete_id(&runtime->context.database,
					tool_context_grant_project_root(tctx), id, deleted);
	rc = reload_after_delete(runtime, rc, *deleted);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return rc;
}

int runtime_permission_revoke_subject(struct runtime *runtime, const char *subject, int *deleted)
{
	struct tool_context *tctx;
	int rc;

	if (!runtime || !subject || !*subject || !deleted)
		return -EINVAL;
	tctx = runtime->context.tctx;
	if (!tctx || !tool_context_grant_project_root(tctx)[0])
		return -ENOENT;
	pthread_mutex_lock(&runtime->context.execution_lock);
	rc = permission_grant_delete_subject(&runtime->context.database,
					     tool_context_grant_project_root(tctx), subject,
					     deleted);
	rc = reload_after_delete(runtime, rc, *deleted);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return rc;
}

int runtime_permission_clear(struct runtime *runtime, int all_projects, int *deleted)
{
	struct tool_context *tctx;
	int rc;

	if (!runtime || !deleted)
		return -EINVAL;
	tctx = runtime->context.tctx;
	if (!tctx || !tool_context_grant_project_root(tctx)[0])
		return -ENOENT;
	pthread_mutex_lock(&runtime->context.execution_lock);
	if (all_projects)
		rc = permission_grant_clear_all(&runtime->context.database, deleted);
	else
		rc = permission_grant_clear_project(&runtime->context.database,
						    tool_context_grant_project_root(tctx), deleted);
	rc = reload_after_delete(runtime, rc, *deleted);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return rc;
}
