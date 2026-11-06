#include "tool_result.h"

#include "util/buf.h"
#include "util/arena.h"
#include "util/error.h"
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

static int tool_result_replace_envelope_field(struct tool_result *result, const char *name,
					      cJSON **slot, cJSON *value)
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
			cJSON_Delete(cJSON_DetachItemViaPointer(result->envelope, old));
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
	return tool_result_replace_envelope_field(result, "meta", &result->meta, meta);
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

			msg = cJSON_IsString(err) ? err->valuestring : "tool failed";
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

int tool_result_error(struct tool_result *result, const char *code, const char *message)
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

int tool_result_errorf(struct tool_result *result, const char *code, const char *fmt, ...)
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
		cJSON_ArrayForEach(item, data)
		{
			if (!item->string || cJSON_GetObjectItem(result->envelope, item->string))
				continue;
			if (cJSON_IsArray(item) || cJSON_IsObject(item))
				continue;
			dup = cJSON_Duplicate(item, 1);
			if (!dup)
				goto oom;
			if (!cJSON_AddItemToObject(result->envelope, item->string, dup)) {
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

int tool_result_add_artifact(struct tool_result *result, enum tool_artifact_kind kind,
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

int tool_result_add_image(struct tool_result *result, const char *path, int width, int height)
{
	int rc = tool_result_add_artifact(result, TOOL_ARTIFACT_IMAGE, path);
	if (rc < 0)
		return rc;
	struct tool_artifact *artifact = &result->artifacts.items[result->artifacts.count - 1];
	artifact->width = width;
	artifact->height = height;
	return 0;
}

int tool_result_add_video(struct tool_result *result, const char *path, int duration_seconds)
{
	int rc = tool_result_add_artifact(result, TOOL_ARTIFACT_VIDEO, path);
	if (rc < 0)
		return rc;
	struct tool_artifact *artifact = &result->artifacts.items[result->artifacts.count - 1];
	artifact->duration_seconds = duration_seconds;
	return 0;
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
		cJSON_ArrayForEach(item, artifacts)
		{
			cJSON *kind = cJSON_GetObjectItem(item, "kind");
			cJSON *path = cJSON_GetObjectItem(item, "path");
			if (!cJSON_IsString(path))
				path = cJSON_GetObjectItem(item, "output_path");
			if (cJSON_IsString(path) && path->valuestring) {
				(void)tool_result_add_artifact(
				    result,
				    tool_artifact_kind_from_string(cJSON_GetStringValue(kind)),
				    path->valuestring);
			}
		}
	} else if (cJSON_IsObject(artifacts)) {
		cJSON *kind = cJSON_GetObjectItem(artifacts, "kind");
		cJSON *path = cJSON_GetObjectItem(artifacts, "path");
		if (!cJSON_IsString(path))
			path = cJSON_GetObjectItem(artifacts, "output_path");
		if (cJSON_IsString(path) && path->valuestring)
			(void)tool_result_add_artifact(
			    result, tool_artifact_kind_from_string(cJSON_GetStringValue(kind)),
			    path->valuestring);
	}
	cJSON_Delete(artifacts);
	return 0;
}
