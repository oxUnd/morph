#ifndef TOOL_H
#define TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>

#define TOOL_NAME_MAX 64
#define TOOL_DESC_MAX 512
#define TOOL_ARGS_SPEC_MAX 1024
#define TOOL_MAX_ENTRIES 64
#define TOOL_DISABLED_MAX 32

#define TOOL_FLAG_READONLY 0x01
#define TOOL_FLAG_INTERNAL_APPROVAL 0x02

struct tool_desc {
	char name[TOOL_NAME_MAX];
	char desc[TOOL_DESC_MAX];
	char args_spec[TOOL_ARGS_SPEC_MAX];
};

typedef int (*tool_exec_fn)(const char *args_json, char **result_json, void *user_data);
typedef void (*tool_user_data_destroy_fn)(void *user_data);

struct tool_entry {
	struct tool_desc desc;
	tool_exec_fn exec;
	void *user_data;
	tool_user_data_destroy_fn user_data_destroy;
	unsigned int flags;
};

struct tool_registry {
	struct tool_entry entries[TOOL_MAX_ENTRIES];
	int count;
	char disabled[TOOL_DISABLED_MAX][TOOL_NAME_MAX];
	int disabled_count;
};

void tool_registry_init(struct tool_registry *reg);
void tool_registry_cleanup(struct tool_registry *reg);
int tool_register(struct tool_registry *reg, const char *name, const char *desc,
		  const char *args_spec, tool_exec_fn exec, void *user_data,
		  tool_user_data_destroy_fn user_data_destroy);
struct tool_entry *tool_lookup(struct tool_registry *reg, const char *name);
int tool_exec(struct tool_registry *reg, const char *name,
	      const char *args_json, char **result_json);
void tool_entry_cleanup_user_data(struct tool_registry *reg);
int tool_disable(struct tool_registry *reg, const char *name);
int tool_is_disabled(struct tool_registry *reg, const char *name);
int tool_is_readonly(struct tool_registry *reg, const char *name);
int tool_has_flag(struct tool_registry *reg, const char *name,
		  unsigned int flag);

#ifdef __cplusplus
}
#endif

#endif
