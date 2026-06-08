#include "tool.h"
#include "util/log.h"
#include <string.h>
#include <stdlib.h>

void tool_registry_init(struct tool_registry *reg)
{
	if (!reg)
		return;
	memset(reg, 0, sizeof(*reg));
	(void)morph_strmap_init(&reg->by_name, TOOL_MAX_ENTRIES);
	(void)morph_strmap_init(&reg->disabled_by_name, TOOL_DISABLED_MAX);
}

void tool_registry_cleanup(struct tool_registry *reg)
{
	tool_entry_cleanup_user_data(reg);
	if (!reg)
		return;
	morph_strmap_cleanup(&reg->by_name);
	morph_strmap_cleanup(&reg->disabled_by_name);
}

void tool_entry_cleanup_user_data(struct tool_registry *reg)
{
	if (!reg)
		return;
	for (int i = 0; i < reg->count; i++) {
		void *ud = reg->entries[i].user_data;
		if (ud && reg->entries[i].user_data_destroy)
			reg->entries[i].user_data_destroy(ud);
		reg->entries[i].user_data = NULL;
	}
}

static int find_tool(struct tool_registry *reg, const char *name)
{
	struct tool_entry *e;

	if (!reg || !name)
		return -1;
	e = (struct tool_entry *)morph_strmap_get(&reg->by_name, name);
	if (e)
		return (int)(e - reg->entries);
	for (int i = 0; i < reg->count; i++) {
		if (strcmp(reg->entries[i].desc.name, name) == 0)
			return i;
	}
	return -1;
}

int tool_register(struct tool_registry *reg, const char *name, const char *desc,
		  const char *args_spec, tool_exec_fn exec, void *user_data,
		  tool_user_data_destroy_fn user_data_destroy)
{
	if (!reg || !name || !exec)
		return -EINVAL;
	if (reg->count >= TOOL_MAX_ENTRIES)
		return -ENOSPC;
	if (find_tool(reg, name) >= 0)
		return -EEXIST;
	struct tool_entry *e = &reg->entries[reg->count];
	memset(e, 0, sizeof(*e));
	strncpy(e->desc.name, name, sizeof(e->desc.name) - 1);
	strncpy(e->desc.desc, desc ? desc : "", sizeof(e->desc.desc) - 1);
	strncpy(e->desc.args_spec, args_spec ? args_spec : "",
		sizeof(e->desc.args_spec) - 1);
	e->exec = exec;
	e->user_data = user_data;
	e->user_data_destroy = user_data_destroy;
	(void)morph_strmap_set(&reg->by_name, e->desc.name, e);
	reg->count++;
	log_dbg("tool registered: %s", name);
	return 0;
}

struct tool_entry *tool_lookup(struct tool_registry *reg, const char *name)
{
	if (!reg || !name)
		return NULL;
	int idx = find_tool(reg, name);
	if (idx < 0)
		return NULL;
	return &reg->entries[idx];
}

int tool_exec(struct tool_registry *reg, const char *name,
	      const char *args_json, char **result_json)
{
	if (!reg || !name || !result_json)
		return -EINVAL;
	struct tool_entry *e = tool_lookup(reg, name);
	if (!e)
		return -ENOENT;
	if (!e->exec)
		return -ENOSYS;
	return e->exec(args_json, result_json, e->user_data);
}

int tool_disable(struct tool_registry *reg, const char *name)
{
	if (!reg || !name)
		return -EINVAL;
	if (reg->disabled_count >= TOOL_DISABLED_MAX)
		return -ENOSPC;
	if (tool_is_disabled(reg, name))
		return 0;
	strncpy(reg->disabled[reg->disabled_count], name, TOOL_NAME_MAX - 1);
	(void)morph_strmap_set(&reg->disabled_by_name,
			       reg->disabled[reg->disabled_count],
			       reg->disabled[reg->disabled_count]);
	reg->disabled_count++;
	log_info("tool disabled: %s", name);
	return 0;
}

int tool_is_disabled(struct tool_registry *reg, const char *name)
{
	if (!reg || !name)
		return 0;
	if (morph_strmap_contains(&reg->disabled_by_name, name))
		return 1;
	for (int i = 0; i < reg->disabled_count; i++) {
		if (strcmp(reg->disabled[i], name) == 0)
			return 1;
	}
	return 0;
}

int tool_is_readonly(struct tool_registry *reg, const char *name)
{
	return tool_has_flag(reg, name, TOOL_FLAG_READONLY);
}

int tool_has_flag(struct tool_registry *reg, const char *name,
		  unsigned int flag)
{
	if (!reg || !name)
		return 0;
	struct tool_entry *e = tool_lookup(reg, name);
	if (!e)
		return 0;
	return (e->flags & flag) != 0;
}
