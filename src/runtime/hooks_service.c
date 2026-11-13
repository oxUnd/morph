#include "runtime/runtime_internal.h"
#include "runtime/hooks.h"
#include "errors.h"
#include "nb/hash.h"
#include "nb/cleanup.h"
#include "cJSON.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int runtime_hooks_snapshot_copy(struct runtime *runtime, cJSON **out)
{
	cJSON *snapshot NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc;

	if (!runtime || !out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	nb_mutex_lock(runtime->context.execution_lock);
	rc = hook_manager_reload(&runtime->context.hooks);
	if (rc == 0) {
		snapshot = hook_manager_list(&runtime->context.hooks);
		if (!snapshot)
			rc = NB_ERR_NOMEM;
	}
	nb_mutex_unlock(runtime->context.execution_lock);
	if (rc == 0) {
		*out = snapshot;
		snapshot = NULL;
	}
	return rc;
}

int runtime_hooks_manage(struct runtime *runtime, const char *action, const char *id,
			 const char *expected_digest)
{
	int rc;

	if (!runtime || !action)
		return NB_ERR_INVAL;
	nb_mutex_lock(runtime->context.execution_lock);
	rc = hook_manager_action(&runtime->context.hooks, action, id, expected_digest);
	nb_mutex_unlock(runtime->context.execution_lock);
	return rc;
}
