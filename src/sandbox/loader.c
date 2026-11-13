#include "loader.h"
#include <stdlib.h>
#include "nb/cleanup.h"
#include "ext.h"
#include "nb/log.h"
#include "errors.h"
#include "nb/file.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dlfcn.h>

int ext_load_so(struct ext *ex, const char *path)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;

	if (!ex || !path)
		return NB_ERR_INVAL;
	if (ex->dl_handle || ex->run)
		return NB_ERR_EXISTS;

	char full_path[PATH_MAX];
	int rc = ((file_status_1 = nb_path_concat(&file_value_1, path, ex->manifest.entry)) != NB_OK
			  ? file_status_1
		  : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), full_path,
						    sizeof(full_path))) == NB_ERR_RANGE
			  ? NB_ERR_NAME_TOO_LONG
			  : file_status_1);
	if (rc < 0)
		return rc;

	struct stat st;
	if (stat(full_path, &st) != 0) {
		nb_log_write(ex ? ex->logger : NULL, NB_LOG_ERROR, "ext_load_so: .so not found: %s",
			     full_path);
		return NB_ERR_NOT_FOUND;
	}

	void *handle = dlopen(full_path, RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		nb_log_write(ex ? ex->logger : NULL, NB_LOG_ERROR,
			     "ext_load_so: dlopen %s failed: %s", full_path, dlerror());
		return MORPH_ERR_LOAD;
	}

	typedef int (*run_fn)(const char *, char **);
	const char *sym_name = ex->manifest.entry;

	void *sym = dlsym(handle, "ext_run");
	run_fn fn = NULL;
	memcpy(&fn, &sym, sizeof(fn));
	if (!fn) {
		const char *slash = strrchr(sym_name, '/');
		const char *basename = slash ? slash + 1 : sym_name;
		char sym_buf[256];
		snprintf(sym_buf, sizeof(sym_buf), "%s_run", basename);
		sym = dlsym(handle, sym_buf);
		memcpy(&fn, &sym, sizeof(fn));
	}

	if (!fn) {
		sym = dlsym(handle, sym_name);
		memcpy(&fn, &sym, sizeof(fn));
	}

	if (!fn) {
		nb_log_write(ex ? ex->logger : NULL, NB_LOG_ERROR,
			     "ext_load_so: no entry symbol found in %s "
			     "(tried ext_run, <entry>_run, <entry>)",
			     full_path);
		dlclose(handle);
		return NB_ERR_NOT_FOUND;
	}

	ex->dl_handle = handle;
	ex->run = fn;
	nb_log_write(ex ? ex->logger : NULL, NB_LOG_INFO,
		     "ext_load_so: loaded %s from %s (handle=%p)", ex->manifest.name, full_path,
		     handle);
	return 0;
}

int ext_load_exec(struct ext *ex, const char *path)
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_2 = NB_OK;

	if (!ex || !path)
		return NB_ERR_INVAL;

	char full_path[PATH_MAX];
	int rc = ((file_status_2 = nb_path_concat(&file_value_2, path, ex->manifest.entry)) != NB_OK
			  ? file_status_2
		  : (file_status_2 = nb_str_copy_to(nb_buf_str(&file_value_2), full_path,
						    sizeof(full_path))) == NB_ERR_RANGE
			  ? NB_ERR_NAME_TOO_LONG
			  : file_status_2);
	if (rc < 0)
		return rc;

	struct stat st;
	if (stat(full_path, &st) != 0) {
		nb_log_write(ex ? ex->logger : NULL, NB_LOG_ERROR,
			     "ext_load_exec: executable not found: %s", full_path);
		return NB_ERR_NOT_FOUND;
	}

	if (!(st.st_mode & S_IXUSR)) {
		nb_log_write(ex ? ex->logger : NULL, NB_LOG_ERROR,
			     "ext_load_exec: not executable: %s", full_path);
		return NB_ERR_PERMISSION;
	}

	snprintf(ex->exec_path, sizeof(ex->exec_path), "%s", full_path);
	nb_log_write(ex ? ex->logger : NULL, NB_LOG_INFO, "ext_load_exec: validated %s -> %s",
		     ex->manifest.name, full_path);
	return 0;
}

void ext_unload_so(struct ext *ex)
{
	if (!ex || !ex->dl_handle)
		return;

	nb_log_write(ex ? ex->logger : NULL, NB_LOG_INFO, "ext_unload_so: dlclose %s (handle=%p)",
		     ex->manifest.name, ex->dl_handle);
	dlclose(ex->dl_handle);
	ex->dl_handle = NULL;
	ex->run = NULL;
}
