#include "tool.h"
#include "util/log.h"
#include <string.h>
#include <stdlib.h>

void tool_registry_init(struct tool_registry *reg)
{
	if (!reg)
		return;
	memset(reg, 0, sizeof(*reg));
}

void tool_registry_cleanup(struct tool_registry *reg)
{
	tool_entry_cleanup_user_data(reg);
}

void tool_entry_cleanup_user_data(struct tool_registry *reg)
{
	if (!reg)
		return;
	for (int i = 0; i < reg->count; i++)
		free(reg->entries[i].user_data);
}

static int find_tool(struct tool_registry *reg, const char *name)
{
	for (int i = 0; i < reg->count; i++) {
		if (strcmp(reg->entries[i].desc.name, name) == 0)
			return i;
	}
	return -1;
}

int tool_register(struct tool_registry *reg, const char *name, const char *desc,
		  const char *args_spec, tool_exec_fn exec, void *user_data)
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