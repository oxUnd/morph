#include "path_policy.h"
#include "agent/tool_context.h"
#include "sandbox/tool_paths.h"
#include "util/error.h"
#include <errno.h>

/* Register the same resolved presets used by the OS sandbox with approvals. */
int tool_path_policy_init(struct tool_context *tctx)
{
	struct sandbox_config cfg = {0};
	struct arena *arena;
	int rc;

	if (!tctx)
		return 0;
	arena = arena_create(0);
	if (!arena)
		return -ENOMEM;
	cfg.path_policy_enabled = 1;
	cfg.allow_temp = 1;
	rc = sandbox_prepare_tool_paths(&cfg, arena);
	for (int i = 0; rc == 0 && i < cfg.read_paths_count; i++)
		rc = tool_context_add_builtin_path(tctx, TOOL_PATH_READ,
			cfg.read_paths[i]);
	for (int i = 0; rc == 0 && i < cfg.write_paths_count; i++)
		rc = tool_context_add_builtin_path(tctx, TOOL_PATH_WRITE,
			cfg.write_paths[i]);
	for (int i = 0; rc == 0 && i < cfg.delete_paths_count; i++)
		rc = tool_context_add_builtin_path(tctx, TOOL_PATH_DELETE,
			cfg.delete_paths[i]);
	arena_destroy(arena);
	return rc;
}

