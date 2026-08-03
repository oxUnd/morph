#include "tool.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/log.h"
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

void tool_result_init(struct tool_result *result)
{
	if (!result)
		return;
	result->envelope = NULL;
	result->text = (morph_str_t)MORPH_STR_NULL;
	result->owned = NULL;
	result->data = NULL;
	result->ui = NULL;
	result->meta = NULL;
	memset(&result->artifacts, 0, sizeof(result->artifacts));
}

void tool_result_clear(struct tool_result *result)
{
	if (!result)
		return;
	free(result->owned);
	if (result->envelope) {
		cJSON_Delete(result->envelope);
	} else {
		cJSON_Delete(result->data);
		cJSON_Delete(result->ui);
		cJSON_Delete(result->meta);
	}
	tool_result_init(result);
}

void tool_result_cleanup(struct tool_result *result)
{
	tool_result_clear(result);
}

static int tool_result_set_owned_text(struct tool_result *result, char *data)
{
	if (!result)
		return -EINVAL;
	if (!data)
		return -ENOMEM;
	free(result->owned);
	result->owned = data;
	result->text.data = data;
	result->text.len = strlen(data);
	return 0;
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

static int tool_result_add_owned(cJSON *object, const char *name, cJSON *value)
{
	if (!object || !name)
		return -EINVAL;
	if (!value)
		return -ENOMEM;
	if (!cJSON_AddItemToObject(object, name, value)) {
		cJSON_Delete(value);
		return -ENOMEM;
	}
	return 0;
}

static int tool_result_replace_envelope_field(struct tool_result *result,
					      const char *name, cJSON **slot,
					      cJSON *value)
{
	cJSON *old;

	if (!result || !name || !slot)
		return -EINVAL;
	if (!value)
		return -ENOMEM;
	if (result->envelope) {
		old = cJSON_GetObjectItem(result->envelope, name);
		if (*slot && *slot != old)
			cJSON_Delete(*slot);
		if (old)
			cJSON_Delete(cJSON_DetachItemViaPointer(result->envelope,
								old));
		*slot = NULL;
		if (!cJSON_AddItemToObject(result->envelope, name, value)) {
			cJSON_Delete(value);
			return -ENOMEM;
		}
		*slot = value;
		return 0;
	}
	cJSON_Delete(*slot);
	*slot = value;
	return 0;
}

int tool_result_set_ui(struct tool_result *result, cJSON *ui)
{
	if (!result)
		return -EINVAL;
	return tool_result_replace_envelope_field(result, "ui", &result->ui, ui);
}

int tool_result_set_meta(struct tool_result *result, cJSON *meta)
{
	if (!result)
		return -EINVAL;
	return tool_result_replace_envelope_field(result, "meta", &result->meta,
						 meta);
}

static int tool_result_set_data(struct tool_result *result, cJSON *data)
{
	if (!result)
		return -EINVAL;
	return tool_result_take_json_field(&result->data, data);
}

static cJSON *tool_result_detach_or_object(cJSON **slot);

int tool_result_success(struct tool_result *result, cJSON *data)
{
	cJSON *item;
	int rc;

	if (!result)
		return -EINVAL;
	tool_result_clear(result);
	rc = tool_result_set_data(result, data ? data : cJSON_CreateObject());
	if (rc != 0)
		return rc;
	result->envelope = cJSON_CreateObject();
	if (!result->envelope) {
		tool_result_clear(result);
		return -ENOMEM;
	}
	if (!cJSON_AddBoolToObject(result->envelope, "ok", 1))
		goto oom;
	item = tool_result_detach_or_object(&result->data);
	if (tool_result_add_owned(result->envelope, "data", item) != 0)
		goto oom;
	return 0;

oom:
	tool_result_clear(result);
	return -ENOMEM;
}

int tool_result_success_text(struct tool_result *result, const char *text)
{
	cJSON *data;

	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	if (!cJSON_AddStringToObject(data, "text", text ? text : "")) {
		cJSON_Delete(data);
		return -ENOMEM;
	}
	return tool_result_success(result, data);
}

int tool_result_successf(struct tool_result *result, const char *fmt, ...)
{
	morph_buf_t msg;
	char *text;
	va_list ap;
	int rc;

	if (!result || !fmt)
		return -EINVAL;
	rc = morph_buf_init(&msg, 128);
	if (rc != 0)
		return rc;
	va_start(ap, fmt);
	rc = morph_buf_vprintf(&msg, fmt, ap);
	va_end(ap);
	if (rc != 0) {
		morph_buf_cleanup(&msg);
		return rc;
	}
	text = morph_buf_detach(&msg);
	if (!text)
		return -ENOMEM;
	return tool_result_success_json_text(result, text);
}

int tool_result_success_json_text(struct tool_result *result, char *json)
{
	cJSON *data;
	cJSON *err;
	const char *msg;
	int rc;

	if (!json)
		return -ENOMEM;
	data = cJSON_Parse(json);
	if (!data) {
		rc = tool_result_success_text(result, json);
		free(json);
		return rc;
	}
	free(json);
	if (cJSON_IsObject(data)) {
		err = cJSON_GetObjectItem(data, "error");
		if (err) {
			cJSON *details;

			msg = cJSON_IsString(err) ? err->valuestring :
				"tool failed";
			tool_result_clear(result);
			result->envelope = cJSON_CreateObject();
			details = data;
			data = NULL;
			err = cJSON_CreateObject();
			if (!result->envelope || !err)
				goto oom_error;
			if (!cJSON_AddBoolToObject(result->envelope, "ok", 0))
				goto oom_error;
			if (!cJSON_AddStringToObject(err, "code", "tool_failed"))
				goto oom_error;
			if (!cJSON_AddStringToObject(err, "message", msg))
				goto oom_error;
			if (tool_result_add_owned(err, "details", details) != 0) {
				details = NULL;
				goto oom_error;
			}
			details = NULL;
			if (tool_result_add_owned(result->envelope, "error", err) != 0) {
				err = NULL;
				goto oom_error;
			}
			return 0;

oom_error:
			cJSON_Delete(details);
			cJSON_Delete(err);
			tool_result_clear(result);
			return -ENOMEM;
		}
	}
	rc = tool_result_success(result, data);
	return rc;
}

int tool_result_error(struct tool_result *result, const char *code,
		      const char *message)
{
	cJSON *details;
	cJSON *err;

	if (!result)
		return -EINVAL;
	tool_result_clear(result);
	result->envelope = cJSON_CreateObject();
	err = cJSON_CreateObject();
	details = cJSON_CreateObject();
	if (!result->envelope || !err)
		goto oom;
	if (!details)
		goto oom;
	if (!cJSON_AddBoolToObject(result->envelope, "ok", 0))
		goto oom;
	if (!cJSON_AddStringToObject(err, "code", code ? code : "tool_failed"))
		goto oom;
	if (!cJSON_AddStringToObject(err, "message", message ? message : ""))
		goto oom;
	if (tool_result_add_owned(err, "details", details) != 0) {
		details = NULL;
		goto oom;
	}
	details = NULL;
	if (tool_result_add_owned(result->envelope, "error", err) != 0) {
		err = NULL;
		goto oom;
	}
	return 0;

oom:
	cJSON_Delete(details);
	cJSON_Delete(err);
	tool_result_clear(result);
	return -ENOMEM;
}

int tool_result_errorf(struct tool_result *result, const char *code,
		       const char *fmt, ...)
{
	morph_buf_t msg;
	va_list ap;
	int rc;

	if (!result || !fmt)
		return -EINVAL;
	rc = morph_buf_init(&msg, 128);
	if (rc != 0)
		return rc;
	va_start(ap, fmt);
	rc = morph_buf_vprintf(&msg, fmt, ap);
	va_end(ap);
	if (rc != 0) {
		morph_buf_cleanup(&msg);
		return rc;
	}
	rc = tool_result_error(result, code, morph_buf_cstr(&msg));
	morph_buf_cleanup(&msg);
	return rc;
}

static cJSON *tool_result_detach_or_object(cJSON **slot)
{
	cJSON *value;

	if (!slot || !*slot)
		return cJSON_CreateObject();
	value = *slot;
	*slot = NULL;
	return value;
}

int tool_result_finalize(struct tool_result *result)
{
	cJSON *artifacts;
	cJSON *data;
	cJSON *dup;
	cJSON *item;
	char *json;

	if (!result)
		return -EINVAL;

	if (!result->envelope) {
		result->envelope = cJSON_CreateObject();
		if (!result->envelope)
			return -ENOMEM;
		if (!cJSON_AddBoolToObject(result->envelope, "ok", 1))
			goto oom;
		item = tool_result_detach_or_object(&result->data);
		if (tool_result_add_owned(result->envelope, "data", item) != 0)
			goto oom;
	}
	artifacts = tool_artifact_list_to_json(&result->artifacts);
	if (!artifacts)
		return -ENOMEM;
	item = cJSON_GetObjectItem(result->envelope, "artifacts");
	if (item) {
		cJSON_DeleteItemFromObject(result->envelope, "artifacts");
	}
	if (tool_result_add_owned(result->envelope, "artifacts", artifacts) != 0)
		goto oom;
	if (!cJSON_GetObjectItem(result->envelope, "ui")) {
		if (result->ui) {
			item = result->ui;
			result->ui = NULL;
			if (tool_result_add_owned(result->envelope, "ui", item) != 0)
				goto oom;
		} else {
			if (!cJSON_AddNullToObject(result->envelope, "ui"))
				goto oom;
		}
	}
	if (!cJSON_GetObjectItem(result->envelope, "meta")) {
		item = tool_result_detach_or_object(&result->meta);
		if (tool_result_add_owned(result->envelope, "meta", item) != 0)
			goto oom;
	}

	result->data = cJSON_GetObjectItem(result->envelope, "data");
	result->ui = cJSON_GetObjectItem(result->envelope, "ui");
	result->meta = cJSON_GetObjectItem(result->envelope, "meta");

	data = cJSON_GetObjectItem(result->envelope, "data");
	if (cJSON_IsObject(data)) {
		cJSON_ArrayForEach(item, data) {
			if (!item->string ||
			    cJSON_GetObjectItem(result->envelope, item->string))
				continue;
			if (cJSON_IsArray(item) || cJSON_IsObject(item))
				continue;
			dup = cJSON_Duplicate(item, 1);
			if (!dup)
				goto oom;
			if (!cJSON_AddItemToObject(result->envelope,
						   item->string, dup)) {
				cJSON_Delete(dup);
				goto oom;
			}
		}
	}

	json = cJSON_PrintUnformatted(result->envelope);
	return tool_result_set_owned_text(result, json);

oom:
	tool_result_clear(result);
	return -ENOMEM;
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

static int tool_schema_validate(const char *schema, int require_closed)
{
	cJSON *root;
	cJSON *type;
	cJSON *props;
	cJSON *required;
	cJSON *item;
	int rc = 0;

	if (!schema || !*schema)
		return -EINVAL;
	root = cJSON_Parse(schema);
	if (!root)
		return MORPH_ERR_PARSE;
	if (!cJSON_IsObject(root)) {
		rc = -EINVAL;
		goto out;
	}
	type = cJSON_GetObjectItem(root, "type");
	props = cJSON_GetObjectItem(root, "properties");
	if (!cJSON_IsString(type) || strcmp(type->valuestring, "object") != 0 ||
	    !cJSON_IsObject(props)) {
		rc = -EINVAL;
		goto out;
	}
	if (require_closed) {
		cJSON *additional = cJSON_GetObjectItem(root,
							"additionalProperties");
		if (!cJSON_IsFalse(additional)) {
			rc = -EINVAL;
			goto out;
		}
	}
	required = cJSON_GetObjectItem(root, "required");
	if (required && cJSON_IsArray(required)) {
		cJSON_ArrayForEach(item, required) {
			if (!cJSON_IsString(item) ||
			    !cJSON_GetObjectItem(props, item->valuestring)) {
				rc = -EINVAL;
				goto out;
			}
		}
	} else if (required) {
		rc = -EINVAL;
	}

out:
	cJSON_Delete(root);
	return rc;
}

int tool_register(struct tool_registry *reg, const struct tool_spec *spec)
{
	int rc;

	if (!reg || !spec || !spec->name || !spec->exec)
		return -EINVAL;
	if (reg->count >= TOOL_MAX_ENTRIES)
		return -ENOSPC;
	if (find_tool(reg, spec->name) >= 0)
		return -EEXIST;
	rc = tool_schema_validate(spec->input_schema, 0);
	if (rc != 0)
		return rc;
	rc = tool_schema_validate(spec->output_schema, 0);
	if (rc != 0)
		return rc;
	struct tool_entry *e = &reg->entries[reg->count];
	memset(e, 0, sizeof(*e));
	strncpy(e->desc.name, spec->name, sizeof(e->desc.name) - 1);
	strncpy(e->desc.title, spec->title ? spec->title : "",
		sizeof(e->desc.title) - 1);
	strncpy(e->desc.description, spec->description ? spec->description : "",
		sizeof(e->desc.description) - 1);
	strncpy(e->desc.input_schema, spec->input_schema,
		sizeof(e->desc.input_schema) - 1);
	strncpy(e->desc.output_schema, spec->output_schema,
		sizeof(e->desc.output_schema) - 1);
	e->exec = spec->exec;
	e->user_data = spec->user_data;
	e->user_data_destroy = spec->user_data_destroy;
	e->origin = spec->origin;
	e->flags = spec->flags;
	e->timeout_seconds = spec->timeout_seconds > 0 ? spec->timeout_seconds : 0;
	(void)morph_strmap_set(&reg->by_name, e->desc.name, e);
	reg->count++;
	log_dbg("tool registered: %s", spec->name);
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
	int result_rc;

	if (!reg || !name || !result)
		return -EINVAL;
	struct tool_entry *e = tool_lookup(reg, name);
	if (!e)
		return -ENOENT;
	if (!e->exec)
		return -ENOSYS;
	int rc = e->exec(args_json, result, e->user_data);
	if (rc < 0 && !result->envelope) {
		const char *code = rc == -EINVAL ? "invalid_arguments" :
			"tool_failed";

		result_rc = tool_result_error(result, code,
					      morph_strerror(rc));
		if (result_rc != 0)
			return result_rc;
	}
	result_rc = tool_result_finalize(result);
	if (result_rc != 0)
		return result_rc;
	return rc;
}

int tool_set_timeout(struct tool_registry *reg, const char *name,
		     int timeout_seconds)
{
	struct tool_entry *e;

	if (!reg || !name)
		return -EINVAL;
	e = tool_lookup(reg, name);
	if (!e)
		return -ENOENT;
	e->timeout_seconds = timeout_seconds > 0 ? timeout_seconds : 0;
	return 0;
}

int tool_timeout_seconds(struct tool_registry *reg, const char *name)
{
	struct tool_entry *e;

	if (!reg || !name)
		return 0;
	e = tool_lookup(reg, name);
	if (!e)
		return 0;
	return e->timeout_seconds;
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
