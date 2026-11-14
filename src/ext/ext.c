#include "nb/signal.h"
#include "sandbox/permission.h"
#include "nb/process.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "ext.h"
#include "loader.h"
#include "nb/file.h"
#include "nb/log.h"
#include "nb/buf.h"
#include "errors.h"
#include "manifest.h"
#include "ipc/jsonrpc.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>

int ext_load(struct nb_log *logger, struct ext *ex, const char *dir_path)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_2 = NB_OK;

	if (!ex || !dir_path)
		return NB_ERR_INVAL;
	if (ex->dl_handle || ex->run || ex->enabled || ex->manifest.input_schema ||
	    ex->manifest.output_schema || ex->manifest.fronts || ex->manifest.categories ||
	    ex->manifest.allowed_paths || ex->manifest.allowed_env ||
	    ex->manifest.allowed_mach_services)
		return NB_ERR_EXISTS;
	struct ext loaded NB_CLEANUP(ext_unload) = {0};
	if (strlen(dir_path) >= sizeof(loaded.path))
		return NB_ERR_NAME_TOO_LONG;
	loaded.logger = logger;
	memcpy(loaded.path, dir_path, strlen(dir_path) + 1);
	char manifest_path[PATH_MAX];
	int rc =
		((file_status_1 = nb_path_concat(&file_value_1, dir_path, "manifest.toml")) != NB_OK
			 ? file_status_1
		 : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), manifest_path,
						   sizeof(manifest_path))) == NB_ERR_RANGE
			 ? NB_ERR_NAME_TOO_LONG
			 : file_status_1);
	if (rc != 0)
		return rc;
	struct stat info;
	if (stat(manifest_path, &info) != 0) {
		rc = ((file_status_2 = nb_path_concat(&file_value_2, dir_path, "morph-ext.toml")) !=
				      NB_OK
			      ? file_status_2
		      : (file_status_2 = nb_str_copy_to(nb_buf_str(&file_value_2), manifest_path,
							sizeof(manifest_path))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_2);
		if (rc != 0)
			return rc;
		if (stat(manifest_path, &info) != 0) {
			rc = nb_status_from_errno(errno, NULL);
			nb_log_write(logger, NB_LOG_ERROR, "ext manifest not found in: %s",
				     dir_path);
			return rc;
		}
	}
	rc = manifest_parse_file(logger, manifest_path, &loaded.manifest);
	if (rc != 0)
		return rc;
	if (strcmp(loaded.manifest.type, "exec") == 0)
		rc = ext_load_exec(&loaded, dir_path);
	else if (strcmp(loaded.manifest.type, "so") == 0)
		rc = ext_load_so(&loaded, dir_path);
	else {
		nb_log_write(logger, NB_LOG_WARN, "unknown ext type: %s", loaded.manifest.type);
		rc = NB_ERR_INVAL;
	}
	if (rc != 0) {
		nb_log_write(logger, NB_LOG_ERROR, "ext_load: failed to load %s: %s",
			     loaded.manifest.name,
			     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	if (!loaded.manifest.input_schema || !loaded.manifest.output_schema)
		return NB_ERR_INVAL;
	loaded.enabled = 1;
	*ex = loaded;
	memset(&loaded, 0, sizeof(loaded));
	/* Embedded descriptor views must refer to the destination's inline arrays. */
	ex->tool_desc.name = ex->manifest.name;
	ex->tool_desc.title = "";
	ex->tool_desc.description = ex->manifest.description;
	ex->tool_desc.input_schema = ex->manifest.input_schema;
	ex->tool_desc.output_schema = ex->manifest.output_schema;
	ex->tool_desc.input_format = "";
	return 0;
}

int ext_unload(struct ext *ex)
{
	if (!ex)
		return NB_ERR_INVAL;
	if (ex->dl_handle) {
		nb_log_write(ex ? ex->logger : NULL, NB_LOG_INFO, "ext_unload: dlclose %s",
			     ex->manifest.name);
		dlclose(ex->dl_handle);
		ex->dl_handle = NULL;
	}
	ext_manifest_cleanup(&ex->manifest);
	memset(ex, 0, sizeof(*ex));
	return 0;
}

void ext_manifest_cleanup(struct ext_manifest *m)
{
	if (!m)
		return;
	free(m->input_schema);
	free(m->output_schema);
	for (int i = 0; i < m->fronts_count; i++)
		free(m->fronts[i]);
	free(m->fronts);
	for (int i = 0; i < m->categories_count; i++)
		free(m->categories[i]);
	free(m->categories);
	for (int i = 0; i < m->allowed_paths_count; i++)
		free(m->allowed_paths[i]);
	free(m->allowed_paths);
	for (int i = 0; i < m->allowed_env_count; i++)
		free(m->allowed_env[i]);
	free(m->allowed_env);
	for (int i = 0; i < m->allowed_mach_services_count; i++)
		free(m->allowed_mach_services[i]);
	free(m->allowed_mach_services);
	memset(m, 0, sizeof(*m));
}

int ext_manifest_supports_front(const struct ext_manifest *m,
				const char *front)
{
	if (!m || !front || !front[0])
		return 0;
	if (m->fronts_count == 0)
		return 1;
	for (int i = 0; i < m->fronts_count; i++) {
		if (m->fronts[i] && strcmp(m->fronts[i], front) == 0)
			return 1;
	}
	return 0;
}

void ext_user_data_destroy(void *user_data)
{
	struct ext *ex = (struct ext *)user_data;
	if (!ex)
		return;
	ext_unload(ex);
	free(ex);
}

static char *ext_resolve_sandbox_path(const char *path)
{
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_5 NB_CLEANUP(nb_buf_destroy) = {0};

	char cwd[PATH_MAX];
	char *expanded;
	char *absolute;
	char *resolved;

	expanded = (nb_path_expand(&file_value_3, path, NULL) == NB_OK
			    ? strdup(nb_buf_cstr(&file_value_3))
			    : NULL);
	if (!expanded)
		return NULL;
	if (nb_path_is_absolute(expanded)) {
		absolute = expanded;
	} else {
		if (!getcwd(cwd, sizeof(cwd))) {
			free(expanded);
			return NULL;
		}
		absolute = (nb_path_join(&file_value_4, cwd, expanded) == NB_OK
				    ? strdup(nb_buf_cstr(&file_value_4))
				    : NULL);
		free(expanded);
		if (!absolute)
			return NULL;
	}
	resolved =
		(nb_path_expand(&file_value_5, absolute, NULL) == NB_OK &&
				 nb_path_canonical_missing(
					 &file_value_5, nb_buf_cstr(&file_value_5), NULL) == NB_OK
			 ? strdup(nb_buf_cstr(&file_value_5))
			 : NULL);
	free(absolute);
	return resolved;
}

void ext_sandbox_config_cleanup(struct sandbox_config *config)
{
	sandbox_config_cleanup(config);
}

int ext_sandbox_config_prepare(const struct ext *ex, struct sandbox_config *config)
{
	if (!ex || !config || ex->manifest.allowed_paths_count < 0)
		return NB_ERR_INVAL;
	if (((void *)config->owned_paths.read.data) || ((void *)config->owned_paths.write.data) ||
	    ((void *)config->owned_paths.remove.data) || config->temporary_paths_ready ||
	    config->temporary_paths.count)
		return NB_ERR_EXISTS;
	struct sandbox_config prepared NB_CLEANUP(sandbox_config_cleanup) = {0};
	struct sandbox_path_set paths NB_CLEANUP(sandbox_path_set_cleanup) = {0};
	int count = ex->manifest.allowed_paths_count;
	int rc = (nb_vec_init(&paths.read, sizeof(*prepared.read_paths)),
		  nb_vec_reserve(&paths.read, (size_t)count + 1) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_vec_init(&paths.write, sizeof(*prepared.read_paths)),
		      nb_vec_reserve(&paths.write, (size_t)count + 1) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_vec_init(&paths.remove, sizeof(*prepared.read_paths)),
		      nb_vec_reserve(&paths.remove, (size_t)count + 1) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	for (int i = 0; i <= count; i++) {
		const char *source = i ? ex->manifest.allowed_paths[i - 1] : ex->path;
		char *resolved NB_CLEANUP(nb_charp_cleanup) = ext_resolve_sandbox_path(source);
		if (!resolved)
			return NB_ERR_NOT_FOUND;
		char **slot = nb_vec_push(&paths.read);
		if (!slot)
			return NB_ERR_NOMEM;
		*slot = nb_charp_steal(&resolved);
		if (i && (ex->manifest.permissions & EXT_PERM_FILESYS)) {
			const char *path = *slot;
			char **write = nb_vec_push(&paths.write);
			if (!write)
				return NB_ERR_NOMEM;
			*write = strdup(path);
			if (!*write)
				return NB_ERR_NOMEM;
			char **remove = nb_vec_push(&paths.remove);
			if (!remove)
				return NB_ERR_NOMEM;
			*remove = strdup(path);
			if (!*remove)
				return NB_ERR_NOMEM;
		}
	}
	prepared.logger = ex->logger;
	prepared.path_policy_enabled = 1;
	sandbox_config_paths_move(&prepared, &paths);
	prepared.permissions = ex->manifest.permissions;
	prepared.path_policy_enabled = 1;
	prepared.network_access = !!(ex->manifest.permissions & EXT_PERM_NETWORK);
	prepared.process_exec = !!(ex->manifest.permissions & EXT_PERM_EXEC);
	prepared.allow_pty = !!(ex->manifest.permissions & EXT_PERM_PTY);
	prepared.allow_process_info = !!(ex->manifest.permissions & EXT_PERM_PROCESS_INFO);
	prepared.allow_ipc = !!(ex->manifest.permissions & EXT_PERM_IPC);
	prepared.allow_temp = !!(ex->manifest.permissions & EXT_PERM_TEMP);
	prepared.allowed_env = ex->manifest.allowed_env;
	prepared.allowed_env_count = ex->manifest.allowed_env_count;
	prepared.allowed_mach_services = ex->manifest.allowed_mach_services;
	prepared.allowed_mach_services_count = ex->manifest.allowed_mach_services_count;
	prepared.max_memory_mb = ex->manifest.max_memory_mb;
	prepared.max_cpu_seconds = ex->manifest.max_cpu_seconds;
	prepared.max_open_files = ex->manifest.max_open_files;
	rc = sandbox_prepare_temporary_paths(&prepared, NULL);
	if (rc != NB_OK)
		return rc;
	*config = prepared;
	memset(&prepared, 0, sizeof(prepared));
	return 0;
}

/**
 * \brief Execute a borrowed extension with optional cooperative cancellation.
 * \param[in] ex: Loaded extension; caller keeps it alive until return.
 * \param[in] args_json: Borrowed JSON input until return.
 * \param[out] result_json: Empty owner slot; caller frees returned JSON, including diagnostics.
 * \param[in] cancelled: Optional synchronous predicate; native code must cooperate.
 * \param[in] user: Borrowed cancellation context until return.
 * \return Zero on success, negative error otherwise; output may contain diagnostics.
 */
static nb_status ext_prepare_child(void *opaque)
{
	struct sandbox_config *config = opaque;
	/* Logger locks belong to parent workers and cannot cross fork. */
	config->logger = NULL;
	int rc = sandbox_enter(config);
	return (nb_status)rc;
}

int ext_run_bounded(struct ext *ex, const char *args_json, char **result_json,
		       int (*cancelled)(void *), void *user, const struct sandbox_config *limits)
{
	if (!ex || !args_json || !result_json)
		return NB_ERR_INVAL;
	if (*result_json)
		return NB_ERR_EXISTS;
	if (cancelled != NULL && cancelled(user)) {
		return NB_ERR_CANCELLED;
	}
	if (!ex->enabled)
		return NB_ERR_PERMISSION;
	if (ex->run)
		return limits ? NB_ERR_UNSUPPORTED : ex->run(args_json, result_json);

	if (strcmp(ex->manifest.type, "exec") == 0 && ex->exec_path[0]) {
		nb_log_write(ex ? ex->logger : NULL, NB_LOG_INFO, "executing ext: %s",
			     ex->exec_path);

		struct sandbox_config sandbox NB_CLEANUP(sandbox_config_cleanup) = {0};
		int rc = ext_sandbox_config_prepare(ex, &sandbox);
		if (rc == NB_OK && limits)
			rc = sandbox_permission_intersect(&sandbox, limits);
		if (rc != NB_OK)
			return rc;
		const char *argv[] = {ex->exec_path, NULL};
		nb_process_options options = {
			.argv = argv,
			.search_path = true,
			.close_fds = true,
			.new_session = true,
			.prepare = ext_prepare_child,
			.prepare_ctx = &sandbox,
		};
		nb_process *child NB_CLEANUP(nb_process_cleanup) = NULL;
		rc = nb_process_spawn(&options, &child, NULL);
		if (rc != NB_OK)
			return rc;

		struct jsonrpc_request req;
		memset(&req, 0, sizeof(req));
		req.id = 1;
		req.method = "run";
		req.params_json = args_json;

		char *request_str NB_CLEANUP(cJSON_FreeTextOwner) = jsonrpc_request_create(&req);
		if (!request_str) {
			*result_json = strdup("{\"error\":\"failed to build JSON-RPC request\"}");
			return NB_ERR_NOMEM;
		}

		nb_buf request NB_CLEANUP(nb_buf_cleanup) = {0};
		nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
		nb_buf errors NB_CLEANUP(nb_buf_cleanup) = {0};
		rc = (nb_buf_init(&request),
		      nb_buf_reserve(&request, strlen(request_str) + 2) ? 0 : NB_ERR_NOMEM);
		if (rc == 0) {
			rc = (nb_buf_append_cstr(&request, request_str) ? 0 : NB_ERR_NOMEM);
		}
		if (rc == 0) {
			rc = (nb_buf_append_char(&request, '\n') ? 0 : NB_ERR_NOMEM);
		}
		if (rc == 0) {
			rc = (nb_buf_init(&output),
			      nb_buf_reserve(&output, BUFSIZ) ? 0 : NB_ERR_NOMEM);
		}
		if (rc == 0) {
			rc = (nb_buf_init(&errors),
			      nb_buf_reserve(&errors, BUFSIZ) ? 0 : NB_ERR_NOMEM);
		}
		cJSON_FreeTextOwner(&request_str);
		int status = 0;
		bool truncated = false;
		nb_process_exchange_options exchange = {
			.timeout_ms = 300000,
			.max_output = 1024u * 1024u,
			.cancelled = cancelled,
			.cancel_ctx = user,
			.stdout_truncated = &truncated,
		};
		if (rc == NB_OK)
			rc = nb_process_exchange(child, nb_buf_cstr(&request), nb_buf_len(&request),
						 &exchange, &output, &errors, &status, NULL);
		if (rc == NB_OK && truncated) {
			*result_json = strdup("{\"error\":\"Extension output exceeds the 1 MiB "
				"capture limit; save large data to a file and return its path\"}");
			return *result_json ? NB_ERR_TOO_BIG : NB_ERR_NOMEM;
		}
		char *raw_response = (nb_buf_ok(&output) ? strdup(nb_buf_cstr(&output)) : NULL);
		char *child_stderr = (nb_buf_ok(&errors) ? strdup(nb_buf_cstr(&errors)) : NULL);

		if (child_stderr && child_stderr[0]) {
			nb_log_write(ex ? ex->logger : NULL, NB_LOG_WARN, "ext %s stderr: %s",
				     ex->manifest.name, child_stderr);
		}

		if (rc < 0 || !raw_response) {
			free(child_stderr);
			free(raw_response);
			*result_json = strdup("{\"error\":\"failed to read ext output\"}");
			return rc < 0 ? rc : NB_ERR_IO;
		}

		/* If the child was killed (sandbox), report that */
		if (nb_process_exit_signal(child) != 0) {
			nb_log_write(ex ? ex->logger : NULL, NB_LOG_WARN,
				     "ext %s killed by signal %d", ex->manifest.name,
				     nb_process_exit_signal(child));
			free(child_stderr);
			free(raw_response);
			*result_json = strdup("{\"error\":\"ext process was terminated\"}");
			return MORPH_ERR_SANDBOX;
		}
		if (nb_process_exit_signal(child) == 0 && status != 0 && raw_response[0] == '\0') {
			int exit_code = status;

			free(child_stderr);
			free(raw_response);
			*result_json = strdup(exit_code == 126 ?
				"{\"error\":\"extension sandbox setup failed\"}" :
				"{\"error\":\"extension process exited before "
				"producing a JSON-RPC response\"}");
			if (exit_code == 126)
				return MORPH_ERR_SANDBOX;
			return NB_ERR_NO_CHILD;
		}
		free(child_stderr);

		/* Parse JSON-RPC response and extract result field */
		struct jsonrpc_response jr = {0};
		int parse_rc = jsonrpc_response_copy(raw_response, &jr);
		free(raw_response);

		if (parse_rc < 0) {
			*result_json = strdup("{\"error\":\"invalid JSON-RPC response\"}");
			return NB_ERR_PARSE;
		}

		if (jr.has_error) {
			cJSON *error = cJSON_CreateObject();
			if (error != NULL &&
			    cJSON_AddStringToObject(error, "error",
						    jr.error_message != NULL
							    ? jr.error_message
							    : "Extension protocol error")) {
				char *text NB_CLEANUP(cJSON_FreeTextOwner) =
					cJSON_PrintUnformatted(error);
				if (text)
					*result_json = strdup(text);
			}
			cJSON_Delete(error);
			jsonrpc_response_free(&jr);
			return *result_json == NULL ? NB_ERR_NOMEM : NB_ERR_PROTOCOL;
		}

		*result_json = strdup(jr.result_json ? jr.result_json : "{}");
		jsonrpc_response_free(&jr);
		return *result_json ? 0 : NB_ERR_NOMEM;
	}

	nb_log_write(ex ? ex->logger : NULL, NB_LOG_INFO, "ext_run: no run function for %s",
		     ex->manifest.name);
	return MORPH_ERR_NOT_CONFIGURED;
}

/** \brief Execute an extension with the legacy non-cancellable contract. */
int ext_run_cancelable(struct ext *ex, const char *args_json, char **result_json,
	int (*cancelled)(void *), void *user)
{
	return ext_run_bounded(ex, args_json, result_json, cancelled, user, NULL);
}

int ext_run(struct ext *ex, const char *args_json, char **result_json)
{
	return ext_run_cancelable(ex, args_json, result_json, NULL, NULL);
}
