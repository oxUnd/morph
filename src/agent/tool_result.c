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
	/* Attached fields detach during free; pending owned fields need the same
	 * cleanup when envelope construction stops before attachment. */
	json_free(result->data);
	json_free(result->ui);
	json_free(result->meta);
	json_free(result->envelope);
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

static int tool_result_validate_owned(const struct tool_result *result, const struct json *value)
{
	const struct json *fields[] = {result->envelope, result->data, result->ui, result->meta};
	int rc = json_validate_take(NULL, value);

	if (rc != 0)
		return rc;
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		if (fields[i]) {
			rc = json_validate_take(fields[i], value);
			if (rc != 0)
				return rc;
		}
	}
	return 0;
}

static int tool_result_take_json_field(struct json **slot, struct json *value)
{
	if (!slot)
		return -EINVAL;
	if (!value)
		return -ENOMEM;
	json_free(*slot);
	*slot = value;
	return 0;
}

static int tool_result_add_owned(struct json *object, const char *name, struct json *value)
{
	int rc;

	if (!object || !name)
		return -EINVAL;
	if (!value)
		return -ENOMEM;
	rc = json_object_add(object, name, value);
	if (rc != 0) {
		json_free(value);
		return rc;
	}
	return 0;
}

static int tool_result_replace_envelope_field(struct tool_result *result, const char *name,
					      struct json **slot, struct json *value)
{
	struct json *old;
	int rc;

	if (!result || !name || !slot)
		return -EINVAL;
	if (!value)
		return -ENOMEM;
	if (result->envelope) {
		old = json_member_ci(result->envelope, name);
		if (value == old) {
			*slot = value;
			return 0;
		}
		rc = old ? json_object_replace_ci(result->envelope, name, value)
			 : json_object_add(result->envelope, name, value);
		if (rc != 0) {
			json_free(value);
			return rc;
		}
		if (*slot && *slot != old)
			json_free(*slot);
		*slot = value;
		return 0;
	}
	if (*slot == value)
		return 0;
	json_free(*slot);
	*slot = value;
	return 0;
}

int tool_result_set_ui(struct tool_result *result, struct json *ui)
{
	int rc;

	if (!result)
		return -EINVAL;
	if (ui && result->ui == ui)
		return 0;
	if (ui) {
		rc = tool_result_validate_owned(result, ui);
		if (rc != 0)
			return rc;
	}
	return tool_result_replace_envelope_field(result, "ui", &result->ui, ui);
}

int tool_result_set_meta(struct tool_result *result, struct json *meta)
{
	int rc;

	if (!result)
		return -EINVAL;
	if (meta && result->meta == meta)
		return 0;
	if (meta) {
		rc = tool_result_validate_owned(result, meta);
		if (rc != 0)
			return rc;
	}
	return tool_result_replace_envelope_field(result, "meta", &result->meta, meta);
}

static int tool_result_set_data(struct tool_result *result, struct json *data)
{
	if (!result)
		return -EINVAL;
	return tool_result_take_json_field(&result->data, data);
}

static struct json *tool_result_detach_or_object(struct json **slot);

int tool_result_success(struct tool_result *result, struct json *data)
{
	struct json *item;
	int rc;

	if (!result)
		return -EINVAL;
	if (data) {
		rc = tool_result_validate_owned(result, data);
		if (rc != 0)
			return rc;
	}
	tool_result_clear(result);
	rc = tool_result_set_data(result, data ? data : json_object());
	if (rc != 0)
		return rc;
	result->envelope = json_object();
	if (!result->envelope) {
		tool_result_clear(result);
		return -ENOMEM;
	}
	if (!json_add_bool(result->envelope, "ok", 1))
		goto oom;
	item = tool_result_detach_or_object(&result->data);
	rc = tool_result_add_owned(result->envelope, "data", item);
	if (rc != 0)
		goto out_clear;
	return 0;

oom:
	rc = -ENOMEM;
out_clear:
	tool_result_clear(result);
	return rc;
}

int tool_result_success_text(struct tool_result *result, const char *text)
{
	struct json *data;

	if (!result)
		return -EINVAL;
	data = json_object();
	if (!data)
		return -ENOMEM;
	if (!json_add_string(data, "text", text ? text : "")) {
		json_free(data);
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
	struct json *data;
	struct json *err;
	const char *msg;
	int rc;

	if (!result) {
		free(json);
		return -EINVAL;
	}
	if (!json)
		return -ENOMEM;
	data = json_parse_prefix(json, NULL, NULL);
	if (!data) {
		rc = tool_result_success_text(result, json);
		free(json);
		return rc;
	}
	free(json);
	if (json_is_object_value(data)) {
		err = json_member_ci(data, "error");
		if (err) {
			struct json *details;

			msg = json_is_string_value(err) ? json_string_value(err) : "tool failed";
			tool_result_clear(result);
			result->envelope = json_object();
			details = data;
			data = NULL;
			err = json_object();
			if (!result->envelope || !err)
				goto oom_error;
			if (!json_add_bool(result->envelope, "ok", 0))
				goto oom_error;
			if (!json_add_string(err, "code", "tool_failed"))
				goto oom_error;
			if (!json_add_string(err, "message", msg))
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
			json_free(details);
			json_free(err);
			tool_result_clear(result);
			return -ENOMEM;
		}
	}
	rc = tool_result_success(result, data);
	return rc;
}

int tool_result_error(struct tool_result *result, const char *code, const char *message)
{
	struct json *envelope;
	struct json *details;
	struct json *err;
	int rc = -ENOMEM;

	if (!result)
		return -EINVAL;
	envelope = json_object();
	err = json_object();
	details = json_object();
	if (!envelope || !err)
		goto out_free_error;
	if (!details)
		goto out_free_error;
	if (!json_add_bool(envelope, "ok", 0))
		goto out_free_error;
	if (!json_add_string(err, "code", code ? code : "tool_failed"))
		goto out_free_error;
	if (!json_add_string(err, "message", message ? message : ""))
		goto out_free_error;
	rc = json_object_add(err, "details", details);
	if (rc != 0)
		goto out_free_error;
	details = NULL;
	rc = json_object_add(envelope, "error", err);
	if (rc != 0)
		goto out_free_error;
	/* The new envelope owns copies before borrowed old-result strings expire. */
	tool_result_clear(result);
	result->envelope = envelope;
	return 0;

out_free_error:
	json_free(details);
	json_free(err);
	json_free(envelope);
	return rc;
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

static struct json *tool_result_detach_or_object(struct json **slot)
{
	struct json *value;

	if (!slot || !*slot)
		return json_object();
	value = *slot;
	*slot = NULL;
	return value;
}

int tool_result_finalize(struct tool_result *result)
{
	struct json *artifacts;
	struct json *data;
	struct json *dup;
	struct json *item;
	char *json;

	if (!result)
		return -EINVAL;

	if (!result->envelope) {
		result->envelope = json_object();
		if (!result->envelope)
			return -ENOMEM;
		if (!json_add_bool(result->envelope, "ok", 1))
			goto oom;
		item = tool_result_detach_or_object(&result->data);
		if (tool_result_add_owned(result->envelope, "data", item) != 0)
			goto oom;
	}
	artifacts = tool_artifact_list_to_json(&result->artifacts);
	if (!artifacts)
		return -ENOMEM;
	item = json_member_ci(result->envelope, "artifacts");
	if (item) {
		json_object_remove_ci(result->envelope, "artifacts");
	}
	if (tool_result_add_owned(result->envelope, "artifacts", artifacts) != 0)
		goto oom;
	if (!json_member_ci(result->envelope, "ui")) {
		if (result->ui) {
			item = result->ui;
			result->ui = NULL;
			if (tool_result_add_owned(result->envelope, "ui", item) != 0)
				goto oom;
		} else {
			if (!json_add_null(result->envelope, "ui"))
				goto oom;
		}
	}
	if (!json_member_ci(result->envelope, "meta")) {
		item = tool_result_detach_or_object(&result->meta);
		if (tool_result_add_owned(result->envelope, "meta", item) != 0)
			goto oom;
	}

	result->data = json_member_ci(result->envelope, "data");
	result->ui = json_member_ci(result->envelope, "ui");
	result->meta = json_member_ci(result->envelope, "meta");

	data = json_member_ci(result->envelope, "data");
	if (json_is_object_value(data)) {
		json_foreach (item, data) {
			if (!json_key(item) || json_member_ci(result->envelope, json_key(item)))
				continue;
			if (json_is_array_value(item) || json_is_object_value(item))
				continue;
			dup = json_clone(item, 1);
			if (!dup)
				goto oom;
			if (json_object_add(result->envelope, json_key(item), dup) != 0) {
				json_free(dup);
				goto oom;
			}
		}
	}

	json = json_stringify(result->envelope);
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

int tool_result_take_artifacts(struct tool_result *result, struct json *artifacts)
{
	struct json *item;

	if (!result)
		return -EINVAL;
	if (!artifacts)
		return -ENOMEM;
	memset(&result->artifacts, 0, sizeof(result->artifacts));
	if (json_is_array_value(artifacts)) {
		json_foreach (item, artifacts) {
			struct json *kind = json_member_ci(item, "kind");
			struct json *path = json_member_ci(item, "path");
			if (!json_is_string_value(path))
				path = json_member_ci(item, "output_path");
			if (json_is_string_value(path) && json_string_value(path)) {
				(void)tool_result_add_artifact(
					result,
					tool_artifact_kind_from_string(json_string_value(kind)),
					json_string_value(path));
			}
		}
	} else if (json_is_object_value(artifacts)) {
		struct json *kind = json_member_ci(artifacts, "kind");
		struct json *path = json_member_ci(artifacts, "path");
		if (!json_is_string_value(path))
			path = json_member_ci(artifacts, "output_path");
		if (json_is_string_value(path) && json_string_value(path))
			(void)tool_result_add_artifact(
				result, tool_artifact_kind_from_string(json_string_value(kind)),
				json_string_value(path));
	}
	json_free(artifacts);
	return 0;
}
