#include "permission.h"
#include "permission/context.h"
#include "sandbox/tool_paths.h"
#include "util/array.h"
#include "util/file.h"
#include <errno.h>
#include <string.h>
#include <limits.h>

static int intersect_paths(char **requested, int count, char **ceiling, int ceiling_count,
	struct arena *arena, char ***out, int *out_count)
{
	morph_array_t paths;
	int rc;

	if (count < 0 || ceiling_count < 0 ||
	    (count && ceiling_count > (INT_MAX - 1) / count))
		return -EOVERFLOW;
	rc = morph_array_init_arena(&paths, arena,
		(size_t)count * (size_t)ceiling_count + 1, sizeof(char *));
	if (rc != 0)
		return rc;
	for (int i = 0; i < count; i++) {
		for (int j = 0; j < ceiling_count; j++) {
			const char *intersection = path_is_within(requested[i], ceiling[j]) ?
				requested[i] : path_is_within(ceiling[j], requested[i]) ?
				ceiling[j] : NULL;
			if (intersection) {
				char **slot = morph_array_push(&paths);

				if (!slot)
					return -ENOMEM;
				*slot = (char *)intersection;
			}
		}
	}
	*out = paths.elts;
	*out_count = (int)paths.nelts;
	return 0;
}

int sandbox_permission_intersect(struct sandbox_config *requested,
	const struct sandbox_config *ceiling, struct arena *arena)
{
	int rc;

	if (!requested || !ceiling || !arena)
		return -EINVAL;
	if (ceiling->read_all) {
		rc = 0;
	} else if (requested->read_all) {
		requested->read_all = 0;
		requested->read_paths = ceiling->read_paths;
		requested->read_paths_count = ceiling->read_paths_count;
		rc = 0;
	} else {
		rc = intersect_paths(requested->read_paths, requested->read_paths_count,
			ceiling->read_paths, ceiling->read_paths_count, arena,
			&requested->read_paths, &requested->read_paths_count);
	}
	if (rc == 0)
		rc = intersect_paths(requested->write_paths, requested->write_paths_count,
		ceiling->write_paths, ceiling->write_paths_count, arena,
		&requested->write_paths, &requested->write_paths_count);
	if (rc == 0)
		rc = intersect_paths(requested->delete_paths, requested->delete_paths_count,
			ceiling->delete_paths, ceiling->delete_paths_count, arena,
			&requested->delete_paths, &requested->delete_paths_count);
	requested->network_access &= ceiling->network_access;
	requested->allow_temp &= ceiling->allow_temp;
	return rc;
}

static int append_path(morph_array_t *paths, struct arena *arena, const char *path)
{
	char **slot;

	if (!path || !*path)
		return 0;
	slot = morph_array_push(paths);
	if (!slot)
		return -ENOMEM;
	*slot = arena_strdup(arena, path);
	return *slot ? 0 : -ENOMEM;
}

int sandbox_permission_prepare(struct tool_context *context, const char *principal,
	const char *scratch, struct arena *arena, struct sandbox_config *out)
{
	struct permission_access_state access;
	struct tool_permission_snapshot snapshot;
	int rc;

	if (!context || !principal || !arena || !out)
		return -EINVAL;
	rc = tool_context_snapshot_permissions(context, principal, arena, &snapshot);
	if (rc != 0)
		return rc;
	access = permission_effective_access(&snapshot.policy);
	memset(out, 0, sizeof(*out));
	out->logger = snapshot.logger;
	out->path_policy_enabled = 1;
	out->read_all = access.host;
	out->network_access = access.network;
	out->process_exec = 1;
	out->allow_process_info = 1;
	out->allow_ipc = 1;
	out->development_runtime = 1;
	out->allow_temp = access.workspace_write;
	for (int operation = TOOL_PATH_READ; operation <= TOOL_PATH_DELETE && rc == 0;
	     operation++) {
		if (operation == TOOL_PATH_LIST)
			continue;
		morph_array_t paths = {0};
		const struct tool_path_snapshot *grants =
			operation == TOOL_PATH_READ    ? &snapshot.read
			: operation == TOOL_PATH_WRITE ? &snapshot.write
						       : &snapshot.remove;
		enum tool_path_op op = (enum tool_path_op)operation;

		rc = morph_array_init_arena(&paths, arena, (size_t)grants->count + 3,
					    sizeof(char *));
		if (rc == 0 && (access.workspace_write || op == TOOL_PATH_READ))
			rc = append_path(&paths, arena, snapshot.workdir);
		if (rc == 0 && (access.workspace_write || op == TOOL_PATH_READ))
			rc = append_path(&paths, arena, snapshot.output_dir);
		if (rc == 0)
			rc = append_path(&paths, arena, scratch);
		for (int i = 0; rc == 0 && i < grants->count; i++)
			rc = append_path(&paths, arena, grants->paths[i]);
		if (op == TOOL_PATH_READ) {
			out->read_paths = paths.elts;
			out->read_paths_count = (int)paths.nelts;
		} else if (op == TOOL_PATH_DELETE) {
			out->delete_paths = paths.elts;
			out->delete_paths_count = (int)paths.nelts;
		} else {
			out->write_paths = paths.elts;
			out->write_paths_count = (int)paths.nelts;
		}
	}
	if (rc == 0 && access.workspace_write)
		rc = sandbox_prepare_tool_paths(out, arena);
	return rc;
}
