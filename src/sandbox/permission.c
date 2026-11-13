#include "permission.h"
#include "nb/err.h"
#include "permission/context.h"
#include "sandbox/tool_paths.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "nb/file.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static int append_path(nb_vec *paths, const char *path)
{
	if (!path || !*path)
		return 0;
	char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(path);
	if (!copy)
		return NB_ERR_NOMEM;
	char **slot = nb_vec_push(paths);
	if (!slot)
		return NB_ERR_NOMEM;
	*slot = nb_charp_steal(&copy);
	return 0;
}

static int intersect_paths(char **requested, int count, char **ceiling, int ceiling_count,
			   nb_vec *out)
{
	if (count < 0 || ceiling_count < 0 || (count && !requested) || (ceiling_count && !ceiling))
		return NB_ERR_INVAL;
	if (count && ceiling_count > (INT_MAX - 1) / count)
		return NB_ERR_RANGE;
	for (int i = 0; i < count; i++) {
		if (!requested[i])
			return NB_ERR_INVAL;
	}
	for (int i = 0; i < ceiling_count; i++) {
		if (!ceiling[i])
			return NB_ERR_INVAL;
	}
	int rc = (nb_vec_init(out, sizeof(*requested)), nb_vec_reserve(out, 16) ? 0 : NB_ERR_NOMEM);
	for (int i = 0; rc == 0 && i < count; i++) {
		for (int j = 0; rc == 0 && j < ceiling_count; j++) {
			const char *intersection =
				nb_path_is_within(requested[i], ceiling[j], true)   ? requested[i]
				: nb_path_is_within(ceiling[j], requested[i], true) ? ceiling[j]
										    : NULL;
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
		return NB_ERR_INVAL;
	struct sandbox_path_set paths NB_CLEANUP(sandbox_path_set_cleanup) = {0};
	int rc;
	if (ceiling->read_all || requested->read_all) {
		const struct sandbox_config *source = ceiling->read_all ? requested : ceiling;
		if (source->read_paths_count < 0 ||
		    (source->read_paths_count && !source->read_paths))
			return NB_ERR_INVAL;
		rc = (nb_vec_init(&paths.read, sizeof(*source->read_paths)),
		      nb_vec_reserve(&paths.read, 16) ? 0 : NB_ERR_NOMEM);
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
		return NB_ERR_INVAL;
	if (((void *)out->owned_paths.read.data) || ((void *)out->owned_paths.write.data) ||
	    ((void *)out->owned_paths.remove.data) || out->temporary_paths_ready ||
	    out->temporary_paths.count)
		return NB_ERR_EXISTS;
	struct tool_permission_snapshot snapshot NB_CLEANUP(tool_permission_snapshot_cleanup) = {0};
	struct sandbox_config prepared NB_CLEANUP(sandbox_config_cleanup) = {0};
	struct sandbox_path_set paths NB_CLEANUP(sandbox_path_set_cleanup) = {0};
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
	nb_vec *arrays[] = {&paths.read, &paths.write, &paths.remove};
	const struct tool_path_snapshot *grants[] = {&snapshot.read, &snapshot.write,
						     &snapshot.remove};
	for (size_t op = 0; op < sizeof(arrays) / sizeof(arrays[0]); op++) {
		rc = (nb_vec_init(arrays[op], sizeof(*grants[op]->paths)),
		      nb_vec_reserve(arrays[op], (size_t)grants[op]->count + 3) ? 0 : NB_ERR_NOMEM);
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
	rc = sandbox_prepare_temporary_paths(&prepared, scratch);
	if (rc == NB_OK && access.workspace_write)
		rc = sandbox_prepare_tool_paths(&prepared);
	if (rc != 0)
		return rc;
	*out = prepared;
	memset(&prepared, 0, sizeof(prepared));
	return 0;
}
