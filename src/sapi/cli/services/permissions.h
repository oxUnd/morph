#ifndef MORPH_CLI_SERVICES_PERMISSIONS_H
#define MORPH_CLI_SERVICES_PERMISSIONS_H

#include "permission/permission.h"
#include <stdint.h>

struct cli_application;
struct runtime_permission_grant;

int cli_permissions_policy_get(struct cli_application *ctx, struct permission_policy *out);
int cli_permissions_policy_update(struct cli_application *ctx,
	const struct permission_policy *policy);
int cli_permissions_grants_list(struct cli_application *ctx,
	struct runtime_permission_grant **out, int *count);
int cli_permissions_grants_revoke(struct cli_application *ctx, int64_t id,
	const char *subject, int *deleted);
int cli_permissions_grants_clear(struct cli_application *ctx, int all_projects, int *deleted);

#endif
