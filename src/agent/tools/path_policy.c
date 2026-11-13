#include "path_policy.h"
#include "agent/tool_context.h"
#include "sandbox/tool_paths.h"
#include "errors.h"
#include "nb/cleanup.h"
#include <errno.h>

/* Register the same resolved presets used by the OS sandbox with approvals. */
int tool_path_policy_init(struct tool_context *tctx)
{
	struct sandbox_config cfg NB_CLEANUP(sandbox_config_cleanup) = {0};
	int rc;

	if (!tctx)
		return 0;
	cfg.path_policy_enabled = 1;
	cfg.allow_temp = 1;
	rc = sandbox_prepare_tool_paths(&cfg);
	for (int i = 0; rc == 0 && i < cfg.read_paths_count; i++)
		rc = tool_context_add_builtin_path(tctx, TOOL_PATH_READ,
			cfg.read_paths[i]);
	for (int i = 0; rc == 0 && i < cfg.write_paths_count; i++)
		rc = tool_context_add_builtin_path(tctx, TOOL_PATH_WRITE,
			cfg.write_paths[i]);
	for (int i = 0; rc == 0 && i < cfg.delete_paths_count; i++)
		rc = tool_context_add_builtin_path(tctx, TOOL_PATH_DELETE,
			cfg.delete_paths[i]);
	const enum tool_path_op operations[] = {TOOL_PATH_READ, TOOL_PATH_WRITE, TOOL_PATH_DELETE};
	for (size_t i = 0; rc == 0 && i < cfg.temporary_paths.count; i++) {
		for (size_t j = 0; rc == 0 && j < sizeof(operations) / sizeof(operations[0]); j++)
			rc = tool_context_add_builtin_path(
				tctx, operations[j], nb_buf_cstr(&cfg.temporary_paths.roots[i]));
	}
	return rc;
}

