#ifndef SKILL_H
#define SKILL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "sandbox.h"
#include "agent/tool.h"

struct skill_manifest {
	char name[64];
	char version[32];
	char description[256];
	char author[64];
	char type[16];
	char entry[128];
	unsigned int permissions;
	char **allowed_paths;
	int allowed_paths_count;
	char **allowed_env;
	int allowed_env_count;
	int max_memory_mb;
	int max_cpu_seconds;
	char *args_schema;
	char *output_schema;
};

struct skill {
	struct skill_manifest manifest;
	char path[512];
	void *dl_handle;
	int (*run)(const char *args_json, char **result_json);
	char exec_path[512];
	struct tool_desc tool_desc;
	int enabled;
};

int skill_load(struct skill *sk, const char *dir_path);
int skill_unload(struct skill *sk);
int skill_run(struct skill *sk, const char *args_json, char **result_json);

#ifdef __cplusplus
}
#endif

#endif