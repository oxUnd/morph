#include "sandbox.h"
#include "util/log.h"
#include <errno.h>
#include <stdlib.h>

int sandbox_enter(struct sandbox_config *cfg)
{
	if (!cfg)
		return -EINVAL;
	log_info("sandbox_enter: perms=0x%x mem=%dMB cpu=%ds",
		 cfg->permissions, cfg->max_memory_mb, cfg->max_cpu_seconds);
	return 0;
}

int sandbox_apply_seccomp(unsigned int permissions)
{
	(void)permissions;
	log_info("sandbox_apply_seccomp: stub");
	return 0;
}

int sandbox_apply_rlimits(int max_memory_mb, int max_cpu_seconds)
{
	(void)max_memory_mb;
	(void)max_cpu_seconds;
	log_info("sandbox_apply_rlimits: stub");
	return 0;
}

int sandbox_apply_fs(const char **allowed_paths, int count)
{
	(void)allowed_paths;
	(void)count;
	log_info("sandbox_apply_fs: stub");
	return 0;
}