#include "loader.h"
#include "util/log.h"
#include <errno.h>

int skill_load_so(struct skill *sk, const char *path)
{
	(void)sk;
	(void)path;
	log_info("skill_load_so: stub");
	return -ENOSYS;
}

int skill_load_exec(struct skill *sk, const char *path)
{
	(void)sk;
	(void)path;
	log_info("skill_load_exec: stub");
	return -ENOSYS;
}

void skill_unload_so(struct skill *sk)
{
	(void)sk;
	log_info("skill_unload_so: stub");
}