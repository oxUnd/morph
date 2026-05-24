#ifndef SANDBOX_H
#define SANDBOX_H

#ifdef __cplusplus
extern "C" {
#endif

#define EXT_PERM_NETWORK	(1 << 0)
#define EXT_PERM_FILESYS	(1 << 1)
#define EXT_PERM_EXEC		(1 << 2)
#define EXT_PERM_ENV		(1 << 3)

struct sandbox_config {
	unsigned int permissions;
	char **allowed_paths;
	int allowed_paths_count;
	char **allowed_env;
	int allowed_env_count;
	int max_memory_mb;
	int max_cpu_seconds;
	int max_file_size_mb;
	int max_processes;
	int max_open_files;
};

int sandbox_enter(struct sandbox_config *cfg);
int sandbox_apply_seccomp(unsigned int permissions);
int sandbox_apply_rlimits(unsigned int permissions, int max_memory_mb,
			  int max_cpu_seconds, int max_file_size_mb,
			  int max_processes, int max_open_files);
int sandbox_apply_fs(const char **allowed_paths, int count,
		     unsigned int permissions);
int sandbox_apply_env(const char **allowed_env, int count,
		      unsigned int permissions);

#ifdef __cplusplus
}
#endif

#endif
