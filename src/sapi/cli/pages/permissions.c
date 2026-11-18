#include "sapi/cli/pages/permissions.h"
#include <nb/err.h>
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/components/selector.h"

static const char *const cli_permissions_presets[] = {"ask", "auto", "full", "read_only"};

static void cli_permissions_apply(struct cli_application *ctx, size_t index)
{
	struct permission_policy policy;
	int rc = runtime_permission_preset(cli_permissions_presets[index], &policy);
	if (!rc)
		rc = runtime_permission_set_policy(ctx->runtime, &policy);
	if (!rc)
		CMD_OK(ctx, "permissions: %s", runtime_permission_preset_name(&policy));
	else
		CMD_ERROR(ctx, "permission change failed: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
}

int cli_permissions_show(struct cli_application *ctx)
{
	static const struct cli_selector_choice choices[] = {
		{"Ask for approval", "Workspace edits; ask before extra access."},
		{"Approve for me", "Model reviews requests for extra access."},
		{"Full Access", "All files and network, without approval."},
		{"Read Only", "Read files; ask before edits or network."},
	};
	struct permission_policy policy;
	int rc = runtime_permission_get_policy(ctx->runtime, &policy);
	if (rc)
		return rc;
	const char *name = runtime_permission_preset_name(&policy);
	int current = -1;
	for (int i = 0; i < 4; i++)
		if (!strcmp(name, cli_permissions_presets[i]))
			current = i;
	char custom[128] = "";
	if (current < 0)
		snprintf(custom, sizeof(custom), "  Custom: %s / %s / %s",
			 permission_access_name(policy.access),
			 permission_approval_name(policy.approval),
			 permission_reviewer_name(policy.reviewer));
	return cli_selector_show(ctx, "  Update Model Permissions", "", *custom ? custom : NULL,
				 choices, 4, current, cli_permissions_apply);
}
