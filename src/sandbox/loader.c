#include "loader.h"
#include "skill.h"
#include "util/log.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dlfcn.h>

int skill_load_so(struct skill *sk, const char *path)
{
	if (!sk || !path)
		return -EINVAL;

	char full_path[1024];
	snprintf(full_path, sizeof(full_path), "%s/%s", path,
		 sk->manifest.entry);

	struct stat st;
	if (stat(full_path, &st) != 0) {
		log_err("skill_load_so: .so not found: %s", full_path);
		return -ENOENT;
	}

	void *handle = dlopen(full_path, RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		log_err("skill_load_so: dlopen %s failed: %s", full_path,
			dlerror());
		return -EIO;
	}

	typedef int (*run_fn)(const char *, char **);
	const char *sym_name = sk->manifest.entry;

	run_fn fn = (run_fn)dlsym(handle, "skill_run");
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
		log_err("skill_load_so: no entry symbol found in %s "
			"(tried skill_run, <entry>_run, <entry>)",
			full_path);
		dlclose(handle);
		return -ENOENT;
	}

	sk->dl_handle = handle;
	sk->run = fn;
	log_info("skill_load_so: loaded %s from %s (handle=%p)",
		 sk->manifest.name, full_path, handle);
	return 0;
}

int skill_load_exec(struct skill *sk, const char *path)
{
	if (!sk || !path)
		return -EINVAL;

	char full_path[1024];
	snprintf(full_path, sizeof(full_path), "%s/%s", path,
		 sk->manifest.entry);

	struct stat st;
	if (stat(full_path, &st) != 0) {
		log_err("skill_load_exec: executable not found: %s", full_path);
		return -ENOENT;
	}

	if (!(st.st_mode & S_IXUSR)) {
		log_err("skill_load_exec: not executable: %s", full_path);
		return -EACCES;
	}

	snprintf(sk->exec_path, sizeof(sk->exec_path), "%s", full_path);
	log_info("skill_load_exec: validated %s -> %s",
		 sk->manifest.name, full_path);
	return 0;
}

void skill_unload_so(struct skill *sk)
{
	if (!sk || !sk->dl_handle)
		return;

	log_info("skill_unload_so: dlclose %s (handle=%p)",
		 sk->manifest.name, sk->dl_handle);
	dlclose(sk->dl_handle);
	sk->dl_handle = NULL;
	sk->run = NULL;
}