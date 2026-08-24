#ifndef SANDBOX_H
#define SANDBOX_H

#ifdef __cplusplus
extern "C" {
#endif

#define EXT_PERM_NETWORK	(1 << 0)
#define EXT_PERM_FILESYS	(1 << 1)
#define EXT_PERM_EXEC		(1 << 2)
#define EXT_PERM_ENV		(1 << 3)
#define EXT_PERM_PTY		(1 << 4)
#define EXT_PERM_PROCESS_INFO	(1 << 5)
#define EXT_PERM_IPC		(1 << 6)
#define EXT_PERM_TEMP		(1 << 7)

struct sandbox_config {
	unsigned int permissions;
	int path_policy_enabled;
	int read_all;
	char **read_paths;
	int read_paths_count;
	char **write_paths;
	int write_paths_count;
	char **delete_paths;
	int delete_paths_count;
	int network_access;
	int process_exec;
	int allow_pty;
	int allow_process_info;
	int allow_ipc;
	int allow_temp;
	char **allowed_paths;
	int allowed_paths_count;
	char **allowed_env;
	int allowed_env_count;
	char **allowed_mach_services;
	int allowed_mach_services_count;
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
int sandbox_start_isolated_session(void);
int sandbox_close_inherited_fds(void);

#ifdef __cplusplus
}
#endif

#endif
