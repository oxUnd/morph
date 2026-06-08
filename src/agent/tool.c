#include "tool.h"
#include "util/buf.h"
#include "util/log.h"
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

void tool_result_init(struct tool_result *result)
{
	if (!result)
		return;
	result->text = (morph_str_t)MORPH_STR_NULL;
	result->owned = NULL;
	result->is_json = 0;
}

void tool_result_clear(struct tool_result *result)
{
	if (!result)
		return;
	free(result->owned);
	tool_result_init(result);
}

void tool_result_cleanup(struct tool_result *result)
{
	tool_result_clear(result);
}

static int tool_result_take_owned(struct tool_result *result, char *data,
				  int is_json)
{
	if (!result)
		return -EINVAL;
	tool_result_clear(result);
	if (!data)
		return -ENOMEM;
	result->owned = data;
	result->text.data = data;
	result->text.len = strlen(data);
	result->is_json = is_json ? 1 : 0;
	return 0;
}

int tool_result_take_text(struct tool_result *result, char *data)
{
	return tool_result_take_owned(result, data, 0);
}

int tool_result_take_json(struct tool_result *result, char *data)
{
	return tool_result_take_owned(result, data, 1);
}

int tool_result_set_textn(struct tool_result *result, const char *data,
			  size_t len)
{
	if (!result || (!data && len > 0))
		return -EINVAL;
	char *copy = malloc(len + 1);
	if (!copy)
		return -ENOMEM;
	if (len > 0)
		memcpy(copy, data, len);
	copy[len] = '\0';
	tool_result_clear(result);
	result->owned = copy;
	result->text.data = copy;
	result->text.len = len;
	result->is_json = 0;
	return 0;
}

int tool_result_set_text(struct tool_result *result, const char *data)
{
	if (!data)
		return tool_result_set_textn(result, "", 0);
	return tool_result_set_textn(result, data, strlen(data));
}

int tool_result_set_json(struct tool_result *result, const char *data)
{
	int rc = tool_result_set_text(result, data);
	if (rc == 0)
		result->is_json = 1;
	return rc;
}

int tool_result_printf(struct tool_result *result, const char *fmt, ...)
{
	if (!result || !fmt)
		return -EINVAL;
	morph_buf_t buf;
	int rc = morph_buf_init(&buf, 256);
	if (rc != 0)
		return rc;
	va_list ap;
	va_start(ap, fmt);
	rc = morph_buf_vprintf(&buf, fmt, ap);
	va_end(ap);
	if (rc != 0) {
		morph_buf_cleanup(&buf);
		return rc;
	}
	return tool_result_take_text(result, morph_buf_detach(&buf));
}

static int tool_result_json_append_string(morph_buf_t *buf, const char *s)
{
	int rc = morph_buf_putc(buf, '"');
	if (rc != 0)
		return rc;
	for (const unsigned char *p = (const unsigned char *)(s ? s : "");
	     *p; p++) {
		switch (*p) {
		case '\\':
			rc = morph_buf_puts(buf, "\\\\");
			break;
		case '"':
			rc = morph_buf_puts(buf, "\\\"");
			break;
		case '\b':
			rc = morph_buf_puts(buf, "\\b");
			break;
		case '\f':
			rc = morph_buf_puts(buf, "\\f");
			break;
		case '\n':
			rc = morph_buf_puts(buf, "\\n");
			break;
		case '\r':
			rc = morph_buf_puts(buf, "\\r");
			break;
		case '\t':
			rc = morph_buf_puts(buf, "\\t");
			break;
		default:
			if (*p < 0x20)
				rc = morph_buf_printf(buf, "\\u%04x", *p);
			else
				rc = morph_buf_putc(buf, (char)*p);
			break;
		}
		if (rc != 0)
			return rc;
	}
	return morph_buf_putc(buf, '"');
}

int tool_result_json_error(struct tool_result *result, const char *message)
{
	if (!result)
		return -EINVAL;
	morph_buf_t buf;
	int rc = morph_buf_init(&buf, 128);
	if (rc != 0)
		return rc;
	rc = morph_buf_puts(&buf, "{\"error\":");
	if (rc == 0)
		rc = tool_result_json_append_string(&buf, message);
	if (rc == 0)
		rc = morph_buf_putc(&buf, '}');
	if (rc != 0) {
		morph_buf_cleanup(&buf);
		return rc;
	}
	return tool_result_take_json(result, morph_buf_detach(&buf));
}

int tool_result_json_errorf(struct tool_result *result, const char *fmt, ...)
{
	if (!result || !fmt)
		return -EINVAL;
	morph_buf_t msg;
	int rc = morph_buf_init(&msg, 128);
	if (rc != 0)
		return rc;
	va_list ap;
	va_start(ap, fmt);
	rc = morph_buf_vprintf(&msg, fmt, ap);
	va_end(ap);
	if (rc != 0) {
		morph_buf_cleanup(&msg);
		return rc;
	}
	rc = tool_result_json_error(result, morph_buf_cstr(&msg));
	morph_buf_cleanup(&msg);
	return rc;
}

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
	      const char *args_json, struct tool_result *result)
{
	if (!reg || !name || !result)
		return -EINVAL;
	struct tool_entry *e = tool_lookup(reg, name);
	if (!e)
		return -ENOENT;
	if (!e->exec)
		return -ENOSYS;
	return e->exec(args_json, result, e->user_data);
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
