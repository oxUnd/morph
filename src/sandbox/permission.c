#include "permission.h"
#include "permission/context.h"
#include "sandbox/tool_paths.h"
#include "util/array.h"
#include "util/file.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static int append_path(morph_array_t *paths, const char *path)
{
	if (!path || !*path)
		return 0;
	char *copy MORPH_CLEANUP(morph_charp_cleanup) = strdup(path);
	if (!copy)
		return -ENOMEM;
	char **slot = morph_array_push_borrow(paths);
	if (!slot)
		return -ENOMEM;
	*slot = morph_charp_steal(&copy);
	return 0;
}

static int intersect_paths(char **requested, int count, char **ceiling, int ceiling_count,
			   morph_array_t *out)
{
	if (count < 0 || ceiling_count < 0 || (count && !requested) || (ceiling_count && !ceiling))
		return -EINVAL;
	if (count && ceiling_count > (INT_MAX - 1) / count)
		return -EOVERFLOW;
	for (int i = 0; i < count; i++) {
		if (!requested[i])
			return -EINVAL;
	}
	for (int i = 0; i < ceiling_count; i++) {
		if (!ceiling[i])
			return -EINVAL;
	}
	int rc = morph_array_init(out, 16, sizeof(*requested));
	for (int i = 0; rc == 0 && i < count; i++) {
		for (int j = 0; rc == 0 && j < ceiling_count; j++) {
			const char *intersection = path_is_within(requested[i], ceiling[j]) ?
				requested[i] : path_is_within(ceiling[j], requested[i]) ?
				ceiling[j] : NULL;
			if (intersection)
				rc = append_path(out, intersection);
		}
	}
	return rc;
}

int sandbox_permission_intersect(struct sandbox_config *requested,
				 const struct sandbox_config *ceiling)
{
	if (!requested || !ceiling)
		return -EINVAL;
	struct sandbox_path_set paths MORPH_CLEANUP(sandbox_path_set_cleanup) = {0};
	int rc;
	if (ceiling->read_all || requested->read_all) {
		const struct sandbox_config *source = ceiling->read_all ? requested : ceiling;
		if (source->read_paths_count < 0 ||
		    (source->read_paths_count && !source->read_paths))
			return -EINVAL;
		rc = morph_array_init(&paths.read, 16, sizeof(*source->read_paths));
		for (int i = 0; rc == 0 && i < source->read_paths_count; i++)
			rc = append_path(&paths.read, source->read_paths[i]);
	} else {
		rc = intersect_paths(requested->read_paths, requested->read_paths_count,
				     ceiling->read_paths, ceiling->read_paths_count, &paths.read);
	}
	if (rc == 0)
		rc = intersect_paths(requested->write_paths, requested->write_paths_count,
				     ceiling->write_paths, ceiling->write_paths_count,
				     &paths.write);
	if (rc == 0)
		rc = intersect_paths(requested->delete_paths, requested->delete_paths_count,
				     ceiling->delete_paths, ceiling->delete_paths_count,
				     &paths.remove);
	if (rc != 0)
		return rc;
	sandbox_config_paths_move(requested, &paths);
	requested->read_all &= ceiling->read_all;
	requested->network_access &= ceiling->network_access;
	requested->allow_temp &= ceiling->allow_temp;
	return 0;
}

int sandbox_permission_prepare(struct tool_context *context, const char *principal,
			       const char *scratch, struct sandbox_config *out)
{
	if (!context || !principal || !out)
		return -EINVAL;
	if (out->owned_paths.read.elts || out->owned_paths.write.elts ||
	    out->owned_paths.remove.elts)
		return -EEXIST;
	struct tool_permission_snapshot snapshot MORPH_CLEANUP(tool_permission_snapshot_cleanup) = {
		0};
	struct sandbox_config prepared MORPH_CLEANUP(sandbox_config_cleanup) = {0};
	struct sandbox_path_set paths MORPH_CLEANUP(sandbox_path_set_cleanup) = {0};
	int rc = tool_context_permission_snapshot_create(context, principal, &snapshot);
	if (rc != 0)
		return rc;
	struct permission_access_state access = permission_effective_access(&snapshot.policy);
	prepared.logger = snapshot.logger;
	prepared.path_policy_enabled = 1;
	prepared.read_all = access.host;
	prepared.network_access = access.network;
	prepared.process_exec = 1;
	prepared.allow_process_info = 1;
	prepared.allow_ipc = 1;
	prepared.development_runtime = 1;
	prepared.allow_temp = access.workspace_write;
	morph_array_t *arrays[] = {&paths.read, &paths.write, &paths.remove};
	const struct tool_path_snapshot *grants[] = {&snapshot.read, &snapshot.write,
						     &snapshot.remove};
	for (size_t op = 0; op < sizeof(arrays) / sizeof(arrays[0]); op++) {
		rc = morph_array_init(arrays[op], (size_t)grants[op]->count + 3,
				      sizeof(*grants[op]->paths));
		if (rc == 0 && (access.workspace_write || op == 0))
			rc = append_path(arrays[op], snapshot.workdir);
		if (rc == 0 && (access.workspace_write || op == 0))
			rc = append_path(arrays[op], snapshot.output_dir);
		if (rc == 0)
			rc = append_path(arrays[op], scratch);
		for (int i = 0; rc == 0 && i < grants[op]->count; i++)
			rc = append_path(arrays[op], grants[op]->paths[i]);
		if (rc != 0)
			return rc;
	}
	sandbox_config_paths_move(&prepared, &paths);
	if (access.workspace_write)
		rc = sandbox_prepare_tool_paths(&prepared);
	if (rc != 0)
		return rc;
	*out = prepared;
	memset(&prepared, 0, sizeof(prepared));
	return 0;
}
