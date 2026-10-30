#include "tool.h"
#include "tool_runtime.h"
#include "util/buf.h"
#include "util/arena.h"
#include <stdint.h>
#include "util/error.h"
#include "util/log.h"
#include "util/utf8.h"
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

void tool_result_init(struct tool_result *result)
{
	if (!result)
		return;
	result->runtime = NULL;
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
	const struct tool_runtime_context *runtime;
	if (!result)
		return;
	runtime = result->runtime;
	free(result->owned);
	if (result->envelope) {
		cJSON_Delete(result->envelope);
	} else {
		cJSON_Delete(result->data);
		cJSON_Delete(result->ui);
		cJSON_Delete(result->meta);
	}
	tool_result_init(result);
	result->runtime = runtime;
}

void tool_result_cleanup(struct tool_result *result)
{
	tool_result_clear(result);
	if (result)
		result->runtime = NULL;
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
	return tool_artifacts_to_json(artifacts ? artifacts->items : NULL,
				      artifacts ? artifacts->count : 0);
}

cJSON *tool_artifacts_to_json(const struct tool_artifact *items, int count)
{
	cJSON *arr;

	arr = cJSON_CreateArray();
	if (!arr)
		return NULL;
	for (int i = 0; items && i < count; i++) {
		const struct tool_artifact *artifact = &items[i];
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

/* Execution references own user data independently of movable registry entries. */
struct tool_binding {
	unsigned int refs;
	void *data;
	tool_user_data_destroy_fn destroy;
};

/** \brief Release one binding; destroy user data only after its final borrower. */
static void tool_binding_release(struct tool_binding *binding)
{
	if (binding != NULL && __atomic_sub_fetch(&binding->refs, 1, __ATOMIC_ACQ_REL) == 0) {
		if (binding->destroy != NULL && binding->data != NULL) {
			binding->destroy(binding->data);
		}
		free(binding);
	}
}

/** \brief Lock raw registry access; recursive for mutation tool callbacks. */
void tool_registry_lock(struct tool_registry *reg)
{
	if (reg != NULL && reg->mutex_ready) {
		pthread_mutex_lock(&reg->mutex);
	}
}

/** \brief End raw registry access. */
void tool_registry_unlock(struct tool_registry *reg)
{
	if (reg != NULL && reg->mutex_ready) {
		pthread_mutex_unlock(&reg->mutex);
	}
}

void tool_registry_init(struct morph_log *logger, struct tool_registry *reg)
{
	if (!reg)
		return;
	memset(reg, 0, sizeof(*reg));
	reg->logger = logger;
	pthread_mutexattr_t attributes;
	int lock_rc = pthread_mutexattr_init(&attributes);
	if (lock_rc == 0) {
		lock_rc = pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
		if (lock_rc == 0) {
			lock_rc = pthread_mutex_init(&reg->mutex, &attributes);
		}
		if (lock_rc == 0) {
			lock_rc = pthread_mutex_init(&reg->mutation_mutex, &attributes);
			if (lock_rc != 0) {
				pthread_mutex_destroy(&reg->mutex);
			}
		}
		pthread_mutexattr_destroy(&attributes);
	}
	reg->mutex_ready = lock_rc == 0;
	reg->init_error = -lock_rc;
	(void)morph_strmap_init(&reg->by_name, TOOL_REGISTRY_INITIAL_CAP);
	(void)morph_strmap_init(&reg->disabled_by_name, TOOL_DISABLED_MAX);
}

void tool_registry_cleanup(struct tool_registry *reg)
{
	tool_entry_cleanup_user_data(reg);
	if (!reg)
		return;
	for (int i = 0; i < reg->count; i++)
		arena_destroy(reg->entries[i].descriptor_arena);
	morph_array_cleanup(&reg->storage);
	reg->entries = NULL;
	reg->count = 0;
	morph_strmap_cleanup(&reg->by_name);
	morph_strmap_cleanup(&reg->disabled_by_name);
	if (reg->mutex_ready) {
		pthread_mutex_destroy(&reg->mutex);
		pthread_mutex_destroy(&reg->mutation_mutex);
	}
	reg->mutex_ready = 0;
}

void tool_entry_cleanup_user_data(struct tool_registry *reg)
{
	if (reg == NULL) {
		return;
	}
	for (int i = 0; i < reg->count; i++) {
		tool_binding_release(reg->entries[i].binding);
		reg->entries[i].binding = NULL;
		reg->entries[i].user_data = NULL;
		reg->entries[i].user_data_destroy = NULL;
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
	    (props && !cJSON_IsObject(props))) {
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
			if (!cJSON_IsString(item) || (require_closed &&
			    !cJSON_GetObjectItem(props, item->valuestring))) {
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

int tool_entry_set_descriptor(struct tool_entry *entry, const struct tool_spec *spec)
{
	if (!entry || !spec || !spec->name)
		return -EINVAL;
	const char *values[] = {spec->name, spec->title, spec->description,
		spec->input_schema, spec->output_schema, spec->input_format};
	struct tool_desc desc = {0};
	const char **fields[] = {&desc.name, &desc.title, &desc.description,
		&desc.input_schema, &desc.output_schema, &desc.input_format};
	size_t bytes = 0;
	struct arena *arena;

	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		size_t len = values[i] ? strlen(values[i]) : 0;

		if (bytes > SIZE_MAX - sizeof(entry->descriptor_arena) ||
		    len > SIZE_MAX - bytes - sizeof(entry->descriptor_arena))
			return -EOVERFLOW;
		bytes += len + sizeof(entry->descriptor_arena);
	}
	arena = arena_create(bytes);
	if (!arena)
		return -ENOMEM;
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		*fields[i] = arena_strdup(arena, values[i] ? values[i] : "");
		if (!*fields[i]) {
			arena_destroy(arena);
			return -ENOMEM;
		}
	}
	desc.input_kind = spec->input_kind;
	arena_destroy(entry->descriptor_arena);
	entry->descriptor_arena = arena;
	entry->desc = desc;
	return 0;
}

static int tool_register_unlocked(struct tool_registry *reg, const struct tool_spec *spec)
{
	int rc;

	if (!reg || !spec || !spec->name || !spec->exec)
		return -EINVAL;
	if (reg->count >= INT_MAX)
		return -EOVERFLOW;
	if (find_tool(reg, spec->name) >= 0)
		return -EEXIST;
	if (spec->input_kind == TOOL_INPUT_JSON) {
		rc = tool_schema_validate(spec->input_schema, 0);
		if (rc != 0)
			return rc;
	} else if (spec->input_kind != TOOL_INPUT_TEXT) {
		return -EINVAL;
	}
	rc = tool_schema_validate(spec->output_schema, 0);
	if (rc != 0)
		return rc;
	struct tool_entry value = {0};
	struct tool_entry *e;

	rc = tool_entry_set_descriptor(&value, spec);
	if (rc != 0)
		return rc;
	if (!reg->storage.elts)
		rc = morph_array_init(&reg->storage, 8, sizeof(value));
	if (rc != 0) {
		arena_destroy(value.descriptor_arena);
		return rc;
	}
	e = morph_array_push(&reg->storage);
	if (!e) {
		arena_destroy(value.descriptor_arena);
		return -ENOMEM;
	}
	reg->entries = reg->storage.elts;
	*e = value;
	e->exec = spec->exec;
	e->get_environment = spec->get_environment;
	e->background_list = spec->background_list;
	e->background_wait = spec->background_wait;
	e->background_cancel = spec->background_cancel;
	e->user_data = spec->user_data;
	e->user_data_destroy = spec->user_data_destroy;
	e->origin = spec->origin;
	e->flags = spec->flags;
	e->timeout_seconds = spec->timeout_seconds > 0 ? spec->timeout_seconds : 0;
	struct tool_binding *binding = calloc(1, sizeof(*binding));
	if (binding == NULL) {
		arena_destroy(e->descriptor_arena);
		memset(e, 0, sizeof(*e));
		morph_array_pop(&reg->storage);
		rebuild_tool_name_index(reg);
		return -ENOMEM;
	}
	binding->refs = 1;
	binding->data = spec->user_data;
	binding->destroy = spec->user_data_destroy;
	e->binding = binding;
	reg->count++;
	rebuild_tool_name_index(reg);
	log_dbg(reg ? reg->logger : NULL, "tool registered: %s", spec->name);
	return 0;
}

static int tool_unregister_unlocked(struct tool_registry *reg, const char *name)
{
	int idx;

	if (!reg || !name)
		return -EINVAL;
	idx = find_tool(reg, name);
	if (idx < 0)
		return -ENOENT;
	struct tool_binding *binding = reg->entries[idx].binding;
	struct arena *descriptor_arena = reg->entries[idx].descriptor_arena;
	for (int i = idx; i < reg->count - 1; i++)
		reg->entries[i] = reg->entries[i + 1];
	memset(&reg->entries[reg->count - 1], 0,
	       sizeof(reg->entries[reg->count - 1]));
	reg->count--;
	morph_array_pop(&reg->storage);
	rebuild_tool_name_index(reg);
	tool_enable_remove_disabled(reg, name);
	log_dbg(reg ? reg->logger : NULL, "tool unregistered: %s", name);
	arena_destroy(descriptor_arena);
	tool_binding_release(binding);
	return 0;
}

/** \brief Register an owned descriptor; user data transfers only on success. */
int tool_register(struct tool_registry *reg, const struct tool_spec *spec)
{
	int rc;
	if (reg == NULL) {
		return -EINVAL;
	}
	if (reg->init_error != 0) {
		return reg->init_error;
	}
	if (!reg->mutex_ready) {
		return MORPH_ERR_NOT_INITIALIZED;
	}
	pthread_mutex_lock(&reg->mutation_mutex);
	tool_registry_lock(reg);
	rc = tool_register_unlocked(reg, spec);
	tool_registry_unlock(reg);
	pthread_mutex_unlock(&reg->mutation_mutex);
	return rc;
}

/** \brief Remove discovery ownership; retained executions remain valid. */
int tool_unregister(struct tool_registry *reg, const char *name)
{
	int rc;
	struct tool_binding *binding = NULL;
	if (reg == NULL) {
		return -EINVAL;
	}
	if (reg->init_error != 0) {
		return reg->init_error;
	}
	if (!reg->mutex_ready) {
		return MORPH_ERR_NOT_INITIALIZED;
	}
	pthread_mutex_lock(&reg->mutation_mutex);
	tool_registry_lock(reg);
	struct tool_entry *entry = tool_lookup(reg, name);
	if (entry != NULL) {
		binding = entry->binding;
		if (binding != NULL) {
			__atomic_add_fetch(&binding->refs, 1, __ATOMIC_RELAXED);
		}
	}
	rc = tool_unregister_unlocked(reg, name);
	tool_registry_unlock(reg);
	pthread_mutex_unlock(&reg->mutation_mutex);
	tool_binding_release(binding);
	return rc;
}

/**
 * \brief Prepare a replacement before atomically publishing its owned binding.
 * \param[in,out] reg: Borrowed registry, internally synchronized.
 * \param[in] spec: Borrowed descriptor; user data transfers only on success.
 * \param[in] replace_flags: Flags required on an existing entry.
 * \return One for replacement, zero for insertion, or a negative error.
 * \note Old executions retain their binding; destruction occurs outside the short lock.
 */
int tool_register_replace(struct tool_registry *reg, const struct tool_spec *spec,
			  unsigned int replace_flags)
{
	struct tool_registry prepared;
	struct tool_entry previous = {0};
	int rc;
	int replaced = 0;
	if (reg == NULL || spec == NULL) {
		return -EINVAL;
	}
	if (reg->init_error != 0) {
		return reg->init_error;
	}
	if (!reg->mutex_ready) {
		return MORPH_ERR_NOT_INITIALIZED;
	}
	tool_registry_init(reg->logger, &prepared);
	rc = tool_register(&prepared, spec);
	if (rc != 0) {
		tool_registry_cleanup(&prepared);
		return rc;
	}
	pthread_mutex_lock(&reg->mutation_mutex);
	tool_registry_lock(reg);
	int index = find_tool(reg, spec->name);
	if (index >= 0) {
		if ((reg->entries[index].flags & replace_flags) != replace_flags) {
			rc = -EEXIST;
		} else {
			previous = reg->entries[index];
			reg->entries[index] = prepared.entries[0];
			memset(&prepared.entries[0], 0, sizeof(prepared.entries[0]));
			rebuild_tool_name_index(reg);
			replaced = 1;
		}
	} else {
		rc = tool_register_unlocked(reg, spec);
	}
	tool_registry_unlock(reg);
	pthread_mutex_unlock(&reg->mutation_mutex);
	/* The candidate's duplicate ownership is never consumed on insertion/failure. */
	if (prepared.entries[0].binding != NULL) {
		prepared.entries[0].binding->destroy = NULL;
	}
	tool_registry_cleanup(&prepared);
	arena_destroy(previous.descriptor_arena);
	tool_binding_release(previous.binding);
	return rc != 0 ? rc : replaced;
}

/**
 * \brief Register a descriptor copy retaining the original user-data binding.
 * \param[in,out] out: Initialized target, exclusively borrowed during copying.
 * \param[in] entry: Borrowed source view; caller keeps it stable until return.
 * \return Zero on success, negative error without consuming the source on failure.
 */
int tool_register_copy(struct tool_registry *out, const struct tool_entry *entry)
{
	if (out == NULL || entry == NULL) {
		return -EINVAL;
	}
	struct tool_spec spec = {
		.origin = entry->origin,
		.name = entry->desc.name,
		.title = entry->desc.title,
		.description = entry->desc.description,
		.input_schema = entry->desc.input_schema,
		.output_schema = entry->desc.output_schema,
		.input_kind = entry->desc.input_kind,
		.input_format = entry->desc.input_format,
		.exec = entry->exec,
		.get_environment = entry->get_environment,
		.background_list = entry->background_list,
		.background_wait = entry->background_wait,
		.background_cancel = entry->background_cancel,
		.user_data = entry->user_data,
		.flags = entry->flags,
		.timeout_seconds = entry->timeout_seconds,
	};
	int rc = tool_register(out, &spec);
	if (rc == 0) {
		struct tool_entry *copy = tool_lookup(out, spec.name);
		tool_binding_release(copy->binding);
		copy->binding = entry->binding;
		if (copy->binding != NULL) {
			__atomic_add_fetch(&copy->binding->refs, 1, __ATOMIC_RELAXED);
		}
	}
	return rc;
}

/**
 * \brief Copy a registry, retaining user data and copying descriptors.
 * \param[in,out] out: Initialized distinct target, exclusively borrowed until return.
 * \param[in] source: Borrowed registry, locked internally for a coherent snapshot.
 * \return Zero on success, negative error otherwise; partial copies require cleanup.
 */
int tool_registry_copy(struct tool_registry *out, struct tool_registry *source)
{
	int rc = 0;
	if (out == NULL || source == NULL || out == source) {
		return -EINVAL;
	}
	tool_registry_lock(source);
	for (int i = 0; i < source->count && rc == 0; i++) {
		const struct tool_entry *entry = &source->entries[i];
		rc = tool_register_copy(out, entry);
	}
	for (int i = 0; i < source->disabled_count && rc == 0; i++) {
		rc = tool_disable(out, source->disabled[i]);
	}
	tool_registry_unlock(source);
	return rc;
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

/** \brief Map a structured business failure to the existing error domain. */
static int tool_error_code(const cJSON *envelope)
{
	const cJSON *error = cJSON_GetObjectItemCaseSensitive(envelope, "error");
	const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "code"));

	if (code != NULL) {
		if (strcmp(code, "invalid_arguments") == 0) {
			return -EINVAL;
		}
		if (strcmp(code, "not_found") == 0) {
			return -ENOENT;
		}
		if (strcmp(code, "permission_denied") == 0) {
			return -EACCES;
		}
		if (strcmp(code, "cancelled") == 0) {
			return -ECANCELED;
		}
		if (strcmp(code, "timeout") == 0) {
			return -ETIMEDOUT;
		}
	}
	return MORPH_ERR_PROCESSING;
}

/**
 * \brief Execute a tool and normalize its business outcome.
 * \param[in] reg: Borrowed registry; execution retains data across concurrent mutation.
 * \param[in] name: Borrowed tool name.
 * \param[in] args_json: Borrowed arguments until return.
 * \param[in,out] result: Initialized caller-owned result, cleared before execution.
 * \return Zero only for business success, otherwise a negative error.
 */
int tool_exec(struct tool_registry *reg, const char *name,
	      const char *args_json, struct tool_result *result)
{
	struct tool_entry *entry;
	struct tool_binding *binding;
	tool_exec_fn execute;
	void *data;
	const cJSON *ok;
	int rc;
	int result_rc;

	if (result == NULL) {
		return -EINVAL;
	}
	tool_result_clear(result);
	if (reg == NULL || name == NULL) {
		return -EINVAL;
	}
	tool_registry_lock(reg);
	entry = tool_lookup(reg, name);
	if (entry == NULL) {
		rc = -ENOENT;
	} else if (tool_is_disabled(reg, name)) {
		rc = -EPERM;
	} else if (entry->exec == NULL) {
		rc = -ENOSYS;
	} else {
		rc = 0;
	}
	if (rc != 0) {
		tool_registry_unlock(reg);
		return rc;
	}
	binding = entry->binding;
	if (binding != NULL) {
		__atomic_add_fetch(&binding->refs, 1, __ATOMIC_RELAXED);
	}
	execute = entry->exec;
	data = entry->user_data;
	tool_registry_unlock(reg);
	rc = execute(args_json, result, data);
	tool_binding_release(binding);
	if (rc > 0) {
		rc = MORPH_ERR_PROTOCOL;
	}
	if (result->envelope != NULL) {
		ok = cJSON_GetObjectItemCaseSensitive(result->envelope, "ok");
		if (!cJSON_IsObject(result->envelope) || !cJSON_IsBool(ok)) {
			if (rc == 0) {
				rc = MORPH_ERR_PROTOCOL;
			}
			tool_result_clear(result);
		} else if (rc == 0 && cJSON_IsFalse(ok)) {
			rc = tool_error_code(result->envelope);
		} else if (rc < 0 && cJSON_IsTrue(ok)) {
			const cJSON *payload =
				cJSON_GetObjectItemCaseSensitive(result->envelope, "data");
			const char *diagnostic = cJSON_GetStringValue(
				cJSON_GetObjectItemCaseSensitive(payload, "text"));
			char *owned = diagnostic != NULL ? strdup(diagnostic) : NULL;
			if (diagnostic != NULL && owned == NULL) {
				tool_result_clear(result);
				return -ENOMEM;
			}
			result_rc = tool_result_error(result, "tool_failed",
						      owned != NULL ? owned : morph_strerror(rc));
			free(owned);
			if (result_rc != 0) {
				return result_rc;
			}
		}
	}
	if (rc < 0 && result->envelope == NULL) {
		result_rc = tool_result_error(result,
					      rc == -EINVAL ? "invalid_arguments" : "tool_failed",
					      morph_strerror(rc));
		if (result_rc != 0) {
			return result_rc;
		}
	}
	result_rc = tool_result_finalize(result);
	return result_rc != 0 ? result_rc : rc;
}

static int tool_set_timeout_locked(struct tool_registry *reg, const char *name, int timeout_seconds)
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

static int tool_timeout_seconds_locked(struct tool_registry *reg, const char *name)
{
	struct tool_entry *e;

	if (!reg || !name)
		return 0;
	e = tool_lookup(reg, name);
	if (!e)
		return 0;
	return e->timeout_seconds;
}

static int tool_disable_locked(struct tool_registry *reg, const char *name)
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
	log_info(reg ? reg->logger : NULL, "tool disabled: %s", name);
	return 0;
}

static int tool_is_disabled_locked(const struct tool_registry *reg, const char *name)
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

static int tool_has_flag_locked(struct tool_registry *reg, const char *name, unsigned int flag)
{
	if (!reg || !name)
		return 0;
	struct tool_entry *e = tool_lookup(reg, name);
	if (!e)
		return 0;
	return (e->flags & flag) != 0;
}

static int tool_is_deferred_locked(struct tool_registry *reg, const char *name)
{
	struct tool_entry *entry = reg ? tool_lookup(reg, name) : NULL;

	if (!entry || !tool_lookup(reg, "tool_search") ||
	    tool_is_disabled(reg, "tool_search") || !strcmp(name, "tool_search"))
		return 0;
	return entry->origin != TOOL_ORIGIN_BUILTIN || (entry->flags & TOOL_FLAG_DEFERRED) ||
		!strncmp(name, "img_", 4) || !strncmp(name, "vid_", 4);
}

static char *tool_discovery_snapshot_locked(struct tool_registry *reg)
{
	if (!reg)
		return NULL;
	cJSON *catalog = cJSON_CreateArray();
	if (!catalog)
		return NULL;
	for (int i = 0; i < reg->count; i++) {
		const struct tool_desc *desc = &reg->entries[i].desc;
		if (tool_is_disabled(reg, desc->name))
			continue;
		cJSON *item = cJSON_CreateObject();
		if (!item || !cJSON_AddStringToObject(item, "name", desc->name) ||
		    !cJSON_AddStringToObject(item, "description", desc->description ?
					 desc->description : "") ||
		    !cJSON_AddItemToArray(catalog, item)) {
			cJSON_Delete(item);
			cJSON_Delete(catalog);
			return NULL;
		}
	}
	char *json = cJSON_PrintUnformatted(catalog);
	cJSON_Delete(catalog);
	return json;
}

static int tool_search_exec(const char *args, struct tool_result *result, void *user_data)
{
	struct tool_registry *reg = user_data;
	cJSON *input = args ? cJSON_ParseWithOpts(args, NULL, 1) : NULL;
	const char *query = cJSON_GetStringValue(cJSON_GetObjectItem(input, "query"));
	cJSON *offset_json = cJSON_GetObjectItem(input, "offset");
	int offset = cJSON_IsNumber(offset_json) ? offset_json->valueint : 0;

	if (!query || offset < 0 || (offset_json && (!cJSON_IsNumber(offset_json) ||
	    offset_json->valuedouble != (double)offset))) {
		cJSON_Delete(input);
		return tool_result_error(result, "invalid_arguments",
			"query is required and offset must be non-negative");
	}
	const char *snapshot = result->runtime ? result->runtime->tool_catalog_json : NULL;
	char *owned_snapshot = snapshot ? NULL : tool_discovery_snapshot(reg);
	if (!snapshot)
		snapshot = owned_snapshot;
	if (!snapshot) {
		cJSON_Delete(input);
		return -ENOMEM;
	}
	cJSON *catalog = cJSON_ParseWithOpts(snapshot, NULL, 1);
	free(owned_snapshot);
	if (!cJSON_IsArray(catalog)) {
		cJSON_Delete(input);
		cJSON_Delete(catalog);
		return MORPH_ERR_PROTOCOL;
	}
	cJSON *data = cJSON_CreateObject();
	cJSON *matches = cJSON_AddArrayToObject(data, "tools");
	int count = 0;
	int total = 0;
	int rc = 0;

	if (!data || !matches) {
		cJSON_Delete(input);
		cJSON_Delete(data);
		cJSON_Delete(catalog);
		return -ENOMEM;
	}
	cJSON *entry;
	cJSON_ArrayForEach(entry, catalog) {
		const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "name"));
		const char *description = cJSON_GetStringValue(
			cJSON_GetObjectItem(entry, "description"));
		if (!name || !description || (query[0] && !strcasestr(name, query) &&
		    !strcasestr(description, query)))
			continue;
		if (total++ < offset || count >= 8)
			continue;
		cJSON *item = cJSON_CreateObject();
		size_t bytes = utf8_clamp_bytes(description, 512);
		char *brief = strndup(description, bytes);

		if (!item || !brief || !cJSON_AddStringToObject(item, "name", name) ||
		    !cJSON_AddStringToObject(item, "description", brief) ||
		    !cJSON_AddItemToArray(matches, item)) {
			cJSON_Delete(item);
			free(brief);
			rc = (-ENOMEM);
			break;
		}
		free(brief);
		count++;
	}
	if (rc == 0 && (!cJSON_AddNumberToObject(data, "total", total) ||
	    !cJSON_AddNumberToObject(data, "next_offset", total > offset + count ?
				    offset + count : -1)))
		rc = (-ENOMEM);
	cJSON_Delete(input);
	cJSON_Delete(catalog);
	if (rc != 0) {
		cJSON_Delete(data);
		return rc;
	}
	return tool_result_success(result, data);
}

int tool_discovery_init(struct tool_registry *reg)
{
	return tool_register(reg, &(struct tool_spec){
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "tool_search",
		.description = "Find tools by a case-insensitive name or description substring. "
			"Use concise English keywords or an exact tool name. "
			"Empty query lists tools; "
			"offset pages through eight matches. Matching tools become available with "
			"their full schemas on the next request. Search before using media, MCP, "
			"extension or dynamic tools whose schemas are not loaded.",
		.input_schema = "{\"type\":\"object\",\"properties\":{"
			"\"query\":{\"type\":\"string\"},"
			"\"offset\":{\"type\":\"integer\",\"minimum\":0}},"
			"\"required\":[\"query\"],\"additionalProperties\":false}",
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = tool_search_exec,
		.user_data = reg,
		.flags = TOOL_FLAG_READONLY,
	});
}

/** \brief Read or update registry metadata under its short data lock. */
int tool_set_timeout(struct tool_registry *reg, const char *name, int timeout_seconds)
{
	tool_registry_lock((struct tool_registry *)reg);
	int value = tool_set_timeout_locked(reg, name, timeout_seconds);
	tool_registry_unlock((struct tool_registry *)reg);
	return value;
}

/** \brief Read or update registry metadata under its short data lock. */
int tool_timeout_seconds(struct tool_registry *reg, const char *name)
{
	tool_registry_lock((struct tool_registry *)reg);
	int value = tool_timeout_seconds_locked(reg, name);
	tool_registry_unlock((struct tool_registry *)reg);
	return value;
}

/** \brief Read or update registry metadata under its short data lock. */
int tool_disable(struct tool_registry *reg, const char *name)
{
	tool_registry_lock((struct tool_registry *)reg);
	int value = tool_disable_locked(reg, name);
	tool_registry_unlock((struct tool_registry *)reg);
	return value;
}

/** \brief Read or update registry metadata under its short data lock. */
int tool_is_disabled(const struct tool_registry *reg, const char *name)
{
	tool_registry_lock((struct tool_registry *)reg);
	int value = tool_is_disabled_locked(reg, name);
	tool_registry_unlock((struct tool_registry *)reg);
	return value;
}

/** \brief Read or update registry metadata under its short data lock. */
int tool_has_flag(struct tool_registry *reg, const char *name, unsigned int flag)
{
	tool_registry_lock((struct tool_registry *)reg);
	int value = tool_has_flag_locked(reg, name, flag);
	tool_registry_unlock((struct tool_registry *)reg);
	return value;
}

/** \brief Read or update registry metadata under its short data lock. */
int tool_is_deferred(struct tool_registry *reg, const char *name)
{
	tool_registry_lock((struct tool_registry *)reg);
	int value = tool_is_deferred_locked(reg, name);
	tool_registry_unlock((struct tool_registry *)reg);
	return value;
}

/**
 * \brief Copy the discovery catalog under the short registry lock.
 * \param[in] reg: Borrowed registry, internally synchronized.
 * \return Owned JSON string to free, or NULL on allocation failure.
 */
char *tool_discovery_snapshot(struct tool_registry *reg)
{
	tool_registry_lock((struct tool_registry *)reg);
	char *value = tool_discovery_snapshot_locked(reg);
	tool_registry_unlock((struct tool_registry *)reg);
	return value;
}
