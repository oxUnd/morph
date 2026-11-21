#include "sapi/cli/services/permissions.h"

#include "sapi/cli/application.h"
#include "sapi/cli/services/sessions.h"
#include "runtime/runtime.h"
#include <nb/cleanup.h>
#include <stdio.h>
#include <stdlib.h>

static int cli_permissions_use_client(const struct cli_application *ctx)
{
	return ctx->sessions && cli_sessions_current_id(ctx) > 0;
}

int cli_permissions_policy_get(struct cli_application *ctx, struct permission_policy *out)
{
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!ctx || !out)
		return NB_ERR_INVAL;

	if (!cli_permissions_use_client(ctx))
		return runtime_permission_get_policy(ctx->runtime, out);

	int rc = cli_sessions_request_current(ctx, "permission.get", NULL, &result);

	if (rc != 0)
		return rc;

	return permission_policy_parse(
		cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "access")),
		cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "approval")),
		cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "reviewer")), out);
}

int cli_permissions_policy_update(struct cli_application *ctx,
	const struct permission_policy *policy)
{
	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!ctx || permission_policy_validate(policy) != 0)
		return NB_ERR_INVAL;

	if (!cli_permissions_use_client(ctx))
		return runtime_permission_set_policy(ctx->runtime, policy);

	if (!params ||
	    !cJSON_AddStringToObject(params, "scope", "session_and_defaults") ||
	    !cJSON_AddStringToObject(params, "access", permission_access_name(policy->access)) ||
	    !cJSON_AddStringToObject(params, "approval",
		permission_approval_name(policy->approval)) ||
	    !cJSON_AddStringToObject(params, "reviewer",
		permission_reviewer_name(policy->reviewer)))
		return NB_ERR_NOMEM;

	return cli_sessions_request_current(ctx, "permission.update", params, &result);
}

static const char *cli_permissions_field(const cJSON *item, const char *name)
{
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, name));

	return text ? text : "";
}

int cli_permissions_grants_list(struct cli_application *ctx,
	struct runtime_permission_grant **out, int *count)
{
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!ctx || !out || !count)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	if (!cli_permissions_use_client(ctx))
		return runtime_permission_list(ctx->runtime, out, count);

	*count = 0;
	int rc = cli_sessions_request_current(ctx, "permission.grants.list", NULL, &result);

	if (rc != 0)
		return rc;

	const cJSON *items = cJSON_GetObjectItemCaseSensitive(result, "grants");
	int length = cJSON_GetArraySize(items);

	if (!cJSON_IsArray(items))
		return NB_ERR_PROTOCOL;

	if (length == 0)
		return 0;

	struct runtime_permission_grant *grants = calloc((size_t)length, sizeof(*grants));

	if (!grants)
		return NB_ERR_NOMEM;

	for (int i = 0; i < length; i++) {
		const cJSON *item = cJSON_GetArrayItem(items, i);

		grants[i].id = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "id"));
		grants[i].created_at = (int64_t)cJSON_GetNumberValue(
			cJSON_GetObjectItem(item, "created_at"));
		snprintf(grants[i].subject, sizeof(grants[i].subject), "%s",
			cli_permissions_field(item, "subject"));
		snprintf(grants[i].resource_kind, sizeof(grants[i].resource_kind), "%s",
			cli_permissions_field(item, "resource_kind"));
		snprintf(grants[i].resource, sizeof(grants[i].resource), "%s",
			cli_permissions_field(item, "resource"));
		snprintf(grants[i].project_root, sizeof(grants[i].project_root), "%s",
			cli_permissions_field(item, "project_root"));
	}

	*out = grants;
	*count = length;
	return 0;
}

int cli_permissions_grants_revoke(struct cli_application *ctx, int64_t id,
	const char *subject, int *deleted)
{
	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!ctx || !deleted)
		return NB_ERR_INVAL;

	if (!cli_permissions_use_client(ctx))
		return subject ? runtime_permission_revoke_subject(ctx->runtime, subject, deleted) :
			runtime_permission_revoke_id(ctx->runtime, id, deleted);

	if (!params || (subject ? !cJSON_AddStringToObject(params, "subject", subject) :
	    !cJSON_AddNumberToObject(params, "grant_id", (double)id)))
		return NB_ERR_NOMEM;

	int rc = cli_sessions_request_current(ctx, "permission.grants.revoke", params, &result);

	if (rc == 0)
		*deleted = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(result, "deleted"));

	return rc;
}

int cli_permissions_grants_clear(struct cli_application *ctx, int all_projects, int *deleted)
{
	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!ctx || !deleted)
		return NB_ERR_INVAL;

	if (!cli_permissions_use_client(ctx))
		return runtime_permission_clear(ctx->runtime, all_projects, deleted);

	if (!params || !cJSON_AddBoolToObject(params, "all_projects", all_projects))
		return NB_ERR_NOMEM;

	int rc = cli_sessions_request_current(ctx, "permission.grants.clear", params, &result);

	if (rc == 0)
		*deleted = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(result, "deleted"));

	return rc;
}
