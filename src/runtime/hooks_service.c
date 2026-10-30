#include "runtime/runtime_internal.h"
#include "runtime/hooks.h"
#include "util/error.h"
#include "util/sha256.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int runtime_hooks_snapshot(struct runtime *runtime, cJSON **out)
{
	int rc;

	if (!runtime || !out)
		return -EINVAL;
	*out = NULL;
	pthread_mutex_lock(&runtime->context.execution_lock);
	rc = hook_manager_reload(&runtime->context.hooks);
	if (rc == 0) {
		*out = hook_manager_list(&runtime->context.hooks);
		if (!*out)
			rc = -ENOMEM;
	}
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return rc;
}

int runtime_hooks_manage(struct runtime *runtime, const char *action, const char *id,
			 const char *expected_digest)
{
	int rc;

	if (!runtime || !action)
		return -EINVAL;
	pthread_mutex_lock(&runtime->context.execution_lock);
	rc = hook_manager_action(&runtime->context.hooks, action, id, expected_digest);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return rc;
}
