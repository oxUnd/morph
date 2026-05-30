#include "loader.h"
#include "ext.h"
#include "util/log.h"
#include "util/error.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dlfcn.h>

int ext_load_so(struct ext *ex, const char *path)
{
	if (!ex || !path)
		return -EINVAL;

	char full_path[PATH_MAX];
	snprintf(full_path, sizeof(full_path), "%s/%s", path,
		 ex->manifest.entry);

	struct stat st;
	if (stat(full_path, &st) != 0) {
		log_err("ext_load_so: .so not found: %s", full_path);
		return -ENOENT;
	}

	void *handle = dlopen(full_path, RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		log_err("ext_load_so: dlopen %s failed: %s", full_path,
			dlerror());
		MORPH_RETURN(MORPH_ERR_LOAD);
	}

	typedef int (*run_fn)(const char *, char **);
	const char *sym_name = ex->manifest.entry;

	run_fn fn = (run_fn)dlsym(handle, "ext_run");
	if (!fn) {
		const char *slash = strrchr(sym_name, '/');
		const char *basename = slash ? slash + 1 : sym_name;
		char sym_buf[256];
		snprintf(sym_buf, sizeof(sym_buf), "%s_run", basename);
		fn = (run_fn)dlsym(handle, sym_buf);
	}

	if (!fn) {
		fn = (run_fn)dlsym(handle, sym_name);
	}

	if (!fn) {
		log_err("ext_load_so: no entry symbol found in %s "
			"(tried ext_run, <entry>_run, <entry>)",
			full_path);
		dlclose(handle);
		return -ENOENT;
	}

	ex->dl_handle = handle;
	ex->run = fn;
	log_info("ext_load_so: loaded %s from %s (handle=%p)",
		 ex->manifest.name, full_path, handle);
	return 0;
}

int ext_load_exec(struct ext *ex, const char *path)
{
	if (!ex || !path)
		return -EINVAL;

	char full_path[PATH_MAX];
	snprintf(full_path, sizeof(full_path), "%s/%s", path,
		 ex->manifest.entry);

	struct stat st;
	if (stat(full_path, &st) != 0) {
		log_err("ext_load_exec: executable not found: %s", full_path);
		return -ENOENT;
	}

	if (!(st.st_mode & S_IXUSR)) {
		log_err("ext_load_exec: not executable: %s", full_path);
		return -EACCES;
	}

	snprintf(ex->exec_path, sizeof(ex->exec_path), "%s", full_path);
	log_info("ext_load_exec: validated %s -> %s",
		 ex->manifest.name, full_path);
	return 0;
}

void ext_unload_so(struct ext *ex)
{
	if (!ex || !ex->dl_handle)
		return;

	log_info("ext_unload_so: dlclose %s (handle=%p)",
		 ex->manifest.name, ex->dl_handle);
	dlclose(ex->dl_handle);
	ex->dl_handle = NULL;
	ex->run = NULL;
}
