#ifndef TOOL_H
#define TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include "cJSON.h"
#include "util/str.h"
#include "util/strmap.h"

#define TOOL_NAME_MAX 512
#define TOOL_DESC_MAX 8192
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

struct tool_result {
	morph_str_t text;
	char *owned;
	int is_json;
	cJSON *data;
	cJSON *ui;
	cJSON *artifacts;
};

void tool_result_init(struct tool_result *result);
void tool_result_cleanup(struct tool_result *result);
void tool_result_clear(struct tool_result *result);
int tool_result_take_text(struct tool_result *result, char *data);
int tool_result_take_json(struct tool_result *result, char *data);
int tool_result_set_text(struct tool_result *result, const char *data);
int tool_result_set_textn(struct tool_result *result, const char *data,
			  size_t len);
int tool_result_set_json(struct tool_result *result, const char *data);
int tool_result_printf(struct tool_result *result, const char *fmt, ...);
int tool_result_json_error(struct tool_result *result, const char *message);
int tool_result_json_errorf(struct tool_result *result, const char *fmt, ...);
int tool_result_take_data(struct tool_result *result, cJSON *data);
int tool_result_take_ui(struct tool_result *result, cJSON *ui);
int tool_result_take_artifacts(struct tool_result *result, cJSON *artifacts);

typedef int (*tool_exec_fn)(const char *args_json,
			    struct tool_result *result,
			    void *user_data);
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
	morph_strmap_t by_name;
	morph_strmap_t disabled_by_name;
};

void tool_registry_init(struct tool_registry *reg);
void tool_registry_cleanup(struct tool_registry *reg);
int tool_register(struct tool_registry *reg, const char *name, const char *desc,
		  const char *args_spec, tool_exec_fn exec, void *user_data,
		  tool_user_data_destroy_fn user_data_destroy);
struct tool_entry *tool_lookup(struct tool_registry *reg, const char *name);
int tool_exec(struct tool_registry *reg, const char *name,
	      const char *args_json, struct tool_result *result);
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
