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
	result->data = NULL;
	result->ui = NULL;
	memset(&result->artifacts, 0, sizeof(result->artifacts));
}

void tool_result_clear(struct tool_result *result)
{
	if (!result)
		return;
	free(result->owned);
	cJSON_Delete(result->data);
	cJSON_Delete(result->ui);
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

static int tool_result_take_json_field(cJSON **slot, cJSON *value)
{
	if (!slot)
		return -EINVAL;
	if (!value)
		return -ENOMEM;
	cJSON_Delete(*slot);
	*slot = value;
	return 0;
}

int tool_result_take_data(struct tool_result *result, cJSON *data)
{
	if (!result)
		return -EINVAL;
	return tool_result_take_json_field(&result->data, data);
}

int tool_result_take_ui(struct tool_result *result, cJSON *ui)
{
	if (!result)
		return -EINVAL;
	return tool_result_take_json_field(&result->ui, ui);
}

const char *tool_artifact_kind_name(enum tool_artifact_kind kind)
{
	switch (kind) {
	case TOOL_ARTIFACT_IMAGE:
		return "image";
	case TOOL_ARTIFACT_VIDEO:
		return "video";
	case TOOL_ARTIFACT_FILE:
	default:
		return "file";
	}
}

enum tool_artifact_kind tool_artifact_kind_from_string(const char *kind)
{
	if (kind && strcmp(kind, "image") == 0)
		return TOOL_ARTIFACT_IMAGE;
	if (kind && strcmp(kind, "video") == 0)
		return TOOL_ARTIFACT_VIDEO;
	return TOOL_ARTIFACT_FILE;
}

int tool_result_add_artifact(struct tool_result *result,
			     enum tool_artifact_kind kind,
			     const char *path)
{
	struct tool_artifact *artifact;

	if (!result || !path || !*path)
		return -EINVAL;
	if (result->artifacts.count >= TOOL_ARTIFACT_MAX)
		return -ENOSPC;
	artifact = &result->artifacts.items[result->artifacts.count++];
	memset(artifact, 0, sizeof(*artifact));
	artifact->kind = kind;
	strncpy(artifact->path, path, sizeof(artifact->path) - 1);
	return 0;
}

int tool_result_add_image(struct tool_result *result, const char *path,
			  int width, int height)
{
	int rc = tool_result_add_artifact(result, TOOL_ARTIFACT_IMAGE, path);
	if (rc < 0)
		return rc;
	struct tool_artifact *artifact =
		&result->artifacts.items[result->artifacts.count - 1];
	artifact->width = width;
	artifact->height = height;
	return 0;
}

int tool_result_add_video(struct tool_result *result, const char *path,
			  int duration_seconds)
{
	int rc = tool_result_add_artifact(result, TOOL_ARTIFACT_VIDEO, path);
	if (rc < 0)
		return rc;
	struct tool_artifact *artifact =
		&result->artifacts.items[result->artifacts.count - 1];
	artifact->duration_seconds = duration_seconds;
	return 0;
}

cJSON *tool_artifact_list_to_json(const struct tool_artifact_list *artifacts)
{
	cJSON *arr;

	arr = cJSON_CreateArray();
	if (!arr)
		return NULL;
	if (!artifacts)
		return arr;
	for (int i = 0; i < artifacts->count; i++) {
		const struct tool_artifact *artifact = &artifacts->items[i];
		cJSON *obj = cJSON_CreateObject();
		if (!obj) {
			cJSON_Delete(arr);
			return NULL;
		}
		cJSON_AddStringToObject(obj, "kind",
					tool_artifact_kind_name(artifact->kind));
		cJSON_AddStringToObject(obj, "path", artifact->path);
		if (artifact->mime[0])
			cJSON_AddStringToObject(obj, "mime", artifact->mime);
		if (artifact->label[0])
			cJSON_AddStringToObject(obj, "label", artifact->label);
		if (artifact->width > 0)
			cJSON_AddNumberToObject(obj, "width", artifact->width);
		if (artifact->height > 0)
			cJSON_AddNumberToObject(obj, "height", artifact->height);
		if (artifact->duration_seconds > 0)
			cJSON_AddNumberToObject(obj, "duration_seconds",
						artifact->duration_seconds);
		if (artifact->size_bytes > 0)
			cJSON_AddNumberToObject(obj, "size_bytes",
						(double)artifact->size_bytes);
		cJSON_AddItemToArray(arr, obj);
	}
	return arr;
}

int tool_result_take_artifacts(struct tool_result *result, cJSON *artifacts)
{
	cJSON *item;

	if (!result)
		return -EINVAL;
	if (!artifacts)
		return -ENOMEM;
	memset(&result->artifacts, 0, sizeof(result->artifacts));
	if (cJSON_IsArray(artifacts)) {
		cJSON_ArrayForEach(item, artifacts) {
			cJSON *kind = cJSON_GetObjectItem(item, "kind");
			cJSON *path = cJSON_GetObjectItem(item, "path");
			if (!cJSON_IsString(path))
				path = cJSON_GetObjectItem(item, "output_path");
			if (cJSON_IsString(path) && path->valuestring) {
				(void)tool_result_add_artifact(result,
					tool_artifact_kind_from_string(
						cJSON_GetStringValue(kind)),
					path->valuestring);
			}
		}
	} else if (cJSON_IsObject(artifacts)) {
		cJSON *kind = cJSON_GetObjectItem(artifacts, "kind");
		cJSON *path = cJSON_GetObjectItem(artifacts, "path");
		if (!cJSON_IsString(path))
			path = cJSON_GetObjectItem(artifacts, "output_path");
		if (cJSON_IsString(path) && path->valuestring)
			(void)tool_result_add_artifact(result,
				tool_artifact_kind_from_string(
					cJSON_GetStringValue(kind)),
				path->valuestring);
	}
	cJSON_Delete(artifacts);
	return 0;
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

const char *tool_origin_name(enum tool_origin origin)
{
	switch (origin) {
	case TOOL_ORIGIN_BUILTIN:
		return "system built-in";
	case TOOL_ORIGIN_DYNAMIC_SESSION:
		return "dynamic session";
	case TOOL_ORIGIN_DYNAMIC_PERSISTENT:
		return "dynamic persistent";
	case TOOL_ORIGIN_MCP:
		return "mcp";
	case TOOL_ORIGIN_EXT:
		return "ext";
	default:
		return "unknown";
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

static void rebuild_tool_name_index(struct tool_registry *reg)
{
	if (!reg)
		return;
	morph_strmap_clear(&reg->by_name);
	for (int i = 0; i < reg->count; i++)
		(void)morph_strmap_set(&reg->by_name,
				       reg->entries[i].desc.name,
				       &reg->entries[i]);
}

static void tool_enable_remove_disabled(struct tool_registry *reg,
					const char *name)
{
	if (!reg || !name)
		return;
	for (int i = 0; i < reg->disabled_count; i++) {
		if (strcmp(reg->disabled[i], name) != 0)
			continue;
		for (int j = i; j < reg->disabled_count - 1; j++) {
			memcpy(reg->disabled[j], reg->disabled[j + 1],
			       sizeof(reg->disabled[j]));
		}
		memset(reg->disabled[reg->disabled_count - 1], 0,
		       sizeof(reg->disabled[reg->disabled_count - 1]));
		reg->disabled_count--;
		morph_strmap_clear(&reg->disabled_by_name);
		for (int j = 0; j < reg->disabled_count; j++) {
			(void)morph_strmap_set(&reg->disabled_by_name,
					       reg->disabled[j],
					       reg->disabled[j]);
		}
		return;
	}
}

int tool_register(enum tool_origin origin, struct tool_registry *reg,
		  const char *name, const char *desc,
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
	e->origin = origin;
	(void)morph_strmap_set(&reg->by_name, e->desc.name, e);
	reg->count++;
	log_dbg("tool registered: %s", name);
	return 0;
}

int tool_unregister(struct tool_registry *reg, const char *name)
{
	int idx;

	if (!reg || !name)
		return -EINVAL;
	idx = find_tool(reg, name);
	if (idx < 0)
		return -ENOENT;
	if (reg->entries[idx].user_data &&
	    reg->entries[idx].user_data_destroy) {
		reg->entries[idx].user_data_destroy(reg->entries[idx].user_data);
	}
	for (int i = idx; i < reg->count - 1; i++)
		reg->entries[i] = reg->entries[i + 1];
	memset(&reg->entries[reg->count - 1], 0,
	       sizeof(reg->entries[reg->count - 1]));
	reg->count--;
	rebuild_tool_name_index(reg);
	tool_enable_remove_disabled(reg, name);
	log_dbg("tool unregistered: %s", name);
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
