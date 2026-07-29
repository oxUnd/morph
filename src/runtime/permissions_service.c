#include "runtime/runtime.h"

#include "db/permission_grant.h"
#include "runtime/runtime_internal.h"
#include "util/array.h"
#include "util/error.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct permission_list_state {
	morph_array_t grants;
};

static int collect_permission(const struct permission_grant *grant,
			      void *user_data)
{
	struct permission_list_state *state = user_data;
	struct runtime_permission_grant *item;

	item = morph_array_push(&state->grants);
	if (!item)
		MORPH_RETURN(-ENOMEM);
	memset(item, 0, sizeof(*item));
	item->id = grant->id;
	item->created_at = grant->created_at;
	snprintf(item->subject, sizeof(item->subject), "%s", grant->subject);
	snprintf(item->resource_kind, sizeof(item->resource_kind), "%s",
		 grant->resource_kind);
	snprintf(item->resource, sizeof(item->resource), "%s",
		 grant->resource);
	snprintf(item->project_root, sizeof(item->project_root), "%s",
		 grant->project_root);
	return 0;
}

int runtime_permission_list(struct runtime *runtime,
			    struct runtime_permission_grant **out,
			    int *count)
{
	struct permission_list_state state;
	struct tool_context *tctx;
	int rc;

	if (!runtime || !out || !count)
		MORPH_RETURN(-EINVAL);
	*out = NULL;
	*count = 0;
	tctx = runtime->context.tctx;
	if (!tctx || !tctx->grant_project_root[0])
		MORPH_RETURN(-ENOENT);
	rc = morph_array_init(&state.grants, 8,
			      sizeof(struct runtime_permission_grant));
	if (rc != 0)
		return rc;
	pthread_mutex_lock(&runtime->context.execution_lock);
	rc = permission_grant_each(&runtime->context.database,
				   tctx->grant_project_root,
				   collect_permission, &state);
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
		rc = tool_context_reload_persistent_grants(
			runtime->context.tctx);
	return rc;
}

int runtime_permission_revoke_id(struct runtime *runtime, int64_t id,
				 int *deleted)
{
	struct tool_context *tctx;
	int rc;

	if (!runtime || !deleted || id <= 0)
		MORPH_RETURN(-EINVAL);
	tctx = runtime->context.tctx;
	if (!tctx || !tctx->grant_project_root[0])
		MORPH_RETURN(-ENOENT);
	pthread_mutex_lock(&runtime->context.execution_lock);
	rc = permission_grant_delete_id(&runtime->context.database,
					tctx->grant_project_root,
					id, deleted);
	rc = reload_after_delete(runtime, rc, *deleted);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return rc;
}

int runtime_permission_revoke_subject(struct runtime *runtime,
				      const char *subject, int *deleted)
{
	struct tool_context *tctx;
	int rc;

	if (!runtime || !subject || !*subject || !deleted)
		MORPH_RETURN(-EINVAL);
	tctx = runtime->context.tctx;
	if (!tctx || !tctx->grant_project_root[0])
		MORPH_RETURN(-ENOENT);
	pthread_mutex_lock(&runtime->context.execution_lock);
	rc = permission_grant_delete_subject(&runtime->context.database,
					     tctx->grant_project_root,
					     subject, deleted);
	rc = reload_after_delete(runtime, rc, *deleted);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return rc;
}

int runtime_permission_clear(struct runtime *runtime, int all_projects,
			     int *deleted)
{
	struct tool_context *tctx;
	int rc;

	if (!runtime || !deleted)
		MORPH_RETURN(-EINVAL);
	tctx = runtime->context.tctx;
	if (!tctx || !tctx->grant_project_root[0])
		MORPH_RETURN(-ENOENT);
	pthread_mutex_lock(&runtime->context.execution_lock);
	if (all_projects)
		rc = permission_grant_clear_all(&runtime->context.database,
						deleted);
	else
		rc = permission_grant_clear_project(
			&runtime->context.database, tctx->grant_project_root,
			deleted);
	rc = reload_after_delete(runtime, rc, *deleted);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return rc;
}
