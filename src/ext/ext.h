#ifndef EXT_H
#define EXT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "sandbox/sandbox.h"
#include "agent/tool.h"

enum ext_purpose {
	EXT_PURPOSE_TOOL      = 0,
	EXT_PURPOSE_GUARDRAIL = 1,
};

struct ext_manifest {
	char name[64];
	char version[32];
	char description[256];
	char author[64];
	char type[16];
	enum ext_purpose purpose;
	char entry[128];
	char hook[32];
	char action_text[512];
	unsigned int permissions;
	char **allowed_paths;
	int allowed_paths_count;
	char **allowed_env;
	int allowed_env_count;
	int max_memory_mb;
	int max_cpu_seconds;
	int max_open_files;
	char *args_schema;
	char *output_schema;
};

struct ext {
	struct ext_manifest manifest;
	char path[512];
	void *dl_handle;
	int (*run)(const char *args_json, char **result_json);
	char exec_path[512];
	struct tool_desc tool_desc;
	int enabled;
};

int ext_load(struct ext *ex, const char *dir_path);
int ext_unload(struct ext *ex);
int ext_run(struct ext *ex, const char *args_json, char **result_json);
void ext_user_data_destroy(void *user_data);

#ifdef __cplusplus
}
#endif

#endif
