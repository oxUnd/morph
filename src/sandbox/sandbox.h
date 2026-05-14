#ifndef SANDBOX_H
#define SANDBOX_H

#ifdef __cplusplus
extern "C" {
#endif

#define SKILL_PERM_NETWORK	(1 << 0)
#define SKILL_PERM_FILESYS	(1 << 1)
#define SKILL_PERM_EXEC		(1 << 2)
#define SKILL_PERM_ENV		(1 << 3)

struct sandbox_config {
	unsigned int permissions;
	char **allowed_paths;
	int allowed_paths_count;
	int max_memory_mb;
	int max_cpu_seconds;
};

int sandbox_enter(struct sandbox_config *cfg);
int sandbox_apply_seccomp(unsigned int permissions);
int sandbox_apply_rlimits(unsigned int permissions, int max_memory_mb, int max_cpu_seconds);
int sandbox_apply_fs(const char **allowed_paths, int count);

#ifdef __cplusplus
}
#endif

#endif