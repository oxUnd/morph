#include "tool_result.h"

#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "cJSON.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void tool_result_init(struct tool_result *result)
{
	if (result)
		memset(result, 0, sizeof(*result));
}

static void tool_result_release_payload(struct tool_result *result)
{
	if (result->owned)
		cJSON_free(result->owned);
	if (result->envelope) {
		cJSON_Delete(result->envelope);
	} else {
		cJSON_Delete(result->data);
		cJSON_Delete(result->ui);
		cJSON_Delete(result->meta);
	}
	result->owned = NULL;
	result->envelope = NULL;
	result->data = NULL;
	result->ui = NULL;
	result->meta = NULL;
	result->text = NB_STR_NULL;
}

void tool_result_clear(struct tool_result *result)
{
	if (!result)
		return;
	tool_result_release_payload(result);
	memset(&result->artifacts, 0, sizeof(result->artifacts));
}

void tool_result_cleanup(struct tool_result *result)
{
	tool_result_clear(result);
	if (result)
		result->runtime = NULL;
}

static void tool_result_commit(struct tool_result *result, cJSON **envelope, int preserve_artifacts)
{
	tool_result_release_payload(result);
	result->envelope = *envelope;
	*envelope = NULL;
	result->data = cJSON_GetObjectItemCaseSensitive(result->envelope, "data");
	result->ui = cJSON_GetObjectItemCaseSensitive(result->envelope, "ui");
	result->meta = cJSON_GetObjectItemCaseSensitive(result->envelope, "meta");
	if (!preserve_artifacts)
		memset(&result->artifacts, 0, sizeof(result->artifacts));
}

static int tool_result_aliases(const struct tool_result *result, const cJSON *value)
{
	return value && (value->prev || value->next || value == result->envelope ||
			 value == result->data || value == result->ui || value == result->meta);
}

/* The key is a static literal. Attaching it cannot allocate a key string. */
static int json_add_copy(cJSON *object, const char *key, const cJSON *value)
{
	cJSON *copy NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Duplicate(value, 1);

	if (!copy)
		return NB_ERR_NOMEM;
	if (!cJSON_AddItemToObjectCS(object, key, copy))
		return NB_ERR_INVAL;
	copy = NULL;
	return 0;
}

static cJSON *tool_result_copy_envelope(const struct tool_result *result)
{
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *empty NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *output;

	if (result->envelope)
		return cJSON_Duplicate(result->envelope, 1);
	root = cJSON_CreateObject();
	if (!root || !cJSON_AddBoolToObject(root, "ok", 1))
		return NULL;
	if (result->data) {
		if (json_add_copy(root, "data", result->data) != 0)
			return NULL;
	} else {
		empty = cJSON_CreateObject();
		if (!empty || !cJSON_AddItemToObjectCS(root, "data", empty))
			return NULL;
		empty = NULL;
	}
	if (result->ui && json_add_copy(root, "ui", result->ui) != 0)
		return NULL;
	if (result->meta && json_add_copy(root, "meta", result->meta) != 0)
		return NULL;
	output = root;
	root = NULL;
	return output;
}

int tool_result_success_take(struct tool_result *result, cJSON **data)
{
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *empty NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!result || !data || data == &result->envelope || data == &result->data ||
	    data == &result->ui || data == &result->meta || tool_result_aliases(result, *data))
		return NB_ERR_INVAL;
	root = cJSON_CreateObject();
	if (!root || !cJSON_AddBoolToObject(root, "ok", 1))
		return NB_ERR_NOMEM;
	if (!*data) {
		empty = cJSON_CreateObject();
		if (!empty)
			return NB_ERR_NOMEM;
	}
	if (!cJSON_AddItemToObjectCS(root, "data", *data ? *data : empty))
		return NB_ERR_INVAL;
	*data = NULL;
	empty = NULL;
	tool_result_commit(result, &root, 0);
	return 0;
}

int tool_result_success_copy(struct tool_result *result, const cJSON *data)
{
	cJSON *copy NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!result)
		return NB_ERR_INVAL;
	if (data) {
		copy = cJSON_Duplicate(data, 1);
		if (!copy)
			return NB_ERR_NOMEM;
	}
	return tool_result_success_take(result, &copy);
}

int tool_result_success_text_copy(struct tool_result *result, const char *text)
{
	cJSON *data NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!result)
		return NB_ERR_INVAL;
	data = cJSON_CreateObject();
	if (!data || !cJSON_AddStringToObject(data, "text", text ? text : ""))
		return NB_ERR_NOMEM;
	return tool_result_success_take(result, &data);
}

static int tool_result_error_details_take(struct tool_result *result, const char *code,
					  const char *message, cJSON **details)
{
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *error NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

	if (!root || !error || !cJSON_AddBoolToObject(root, "ok", 0) ||
	    !cJSON_AddStringToObject(error, "code", code ? code : "tool_failed") ||
	    !cJSON_AddStringToObject(error, "message", message ? message : ""))
		return NB_ERR_NOMEM;
	/* All fallible work is complete before the incoming node is attached. */
	if (!cJSON_AddItemToObjectCS(root, "error", error))
		return NB_ERR_INVAL;
	cJSON *attached_error = error;

	error = NULL;
	if (!cJSON_AddItemToObjectCS(attached_error, "details", *details))
		return NB_ERR_INVAL;
	*details = NULL;
	tool_result_commit(result, &root, 0);
	return 0;
}

int tool_result_success_json_text_copy(struct tool_result *result, const char *json)
{
	cJSON *data NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!result)
		return NB_ERR_INVAL;
	if (!json)
		return NB_ERR_NOMEM;
	data = cJSON_Parse(json);
	if (!data)
		return tool_result_success_text_copy(result, json);
	if (cJSON_IsObject(data)) {
		const cJSON *error = cJSON_GetObjectItemCaseSensitive(data, "error");

		if (error) {
			const char *message =
				cJSON_IsString(error) ? error->valuestring : "tool failed";

			return tool_result_error_details_take(result, "tool_failed", message,
							      &data);
		}
	}
	return tool_result_success_take(result, &data);
}

int tool_result_success_json_text_take(struct tool_result *result, char **json)
{
	int rc;

	if (!result || !json || json == &result->owned || (*json && *json == result->owned))
		return NB_ERR_INVAL;
	rc = tool_result_success_json_text_copy(result, *json);
	if (rc == 0) {
		free(*json);
		*json = NULL;
	}
	return rc;
}

int tool_result_success_format(struct tool_result *result, const char *fmt, ...)
{
	nb_buf message NB_CLEANUP(nb_buf_cleanup) = {0};
	va_list arguments;
	int rc;

	if (!result || !fmt)
		return NB_ERR_INVAL;
	rc = (nb_buf_init(&message), nb_buf_reserve(&message, 128) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	va_start(arguments, fmt);
	rc = (nb_buf_vprintf(&message, fmt, arguments) ? 0 : NB_ERR_NOMEM);
	va_end(arguments);
	if (rc != 0)
		return rc;
	return tool_result_success_json_text_copy(result, nb_buf_cstr(&message));
}

int tool_result_error_copy(struct tool_result *result, const char *code, const char *message)
{
	cJSON *details NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!result)
		return NB_ERR_INVAL;
	details = cJSON_CreateObject();
	if (!details)
		return NB_ERR_NOMEM;
	return tool_result_error_details_take(result, code, message, &details);
}

int tool_result_error_format(struct tool_result *result, const char *code, const char *fmt, ...)
{
	nb_buf message NB_CLEANUP(nb_buf_cleanup) = {0};
	va_list arguments;
	int rc;

	if (!result || !fmt)
		return NB_ERR_INVAL;
	rc = (nb_buf_init(&message), nb_buf_reserve(&message, 128) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	va_start(arguments, fmt);
	rc = (nb_buf_vprintf(&message, fmt, arguments) ? 0 : NB_ERR_NOMEM);
	va_end(arguments);
	if (rc != 0)
		return rc;
	return tool_result_error_copy(result, code, nb_buf_cstr(&message));
}

static int tool_result_field_take(struct tool_result *result, const char *key, cJSON **value)
{
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!result || !value || !*value || value == &result->envelope || value == &result->data ||
	    value == &result->ui || value == &result->meta || tool_result_aliases(result, *value))
		return NB_ERR_INVAL;
	root = tool_result_copy_envelope(result);
	if (!root)
		return NB_ERR_NOMEM;
	cJSON_DeleteItemFromObjectCaseSensitive(root, key);
	if (!cJSON_AddItemToObjectCS(root, key, *value))
		return NB_ERR_INVAL;
	*value = NULL;
	tool_result_commit(result, &root, 1);
	return 0;
}

int tool_result_set_ui_take(struct tool_result *result, cJSON **ui)
{
	return tool_result_field_take(result, "ui", ui);
}

int tool_result_set_meta_take(struct tool_result *result, cJSON **meta)
{
	return tool_result_field_take(result, "meta", meta);
}

static int tool_result_field_copy(struct tool_result *result, const char *key, const cJSON *value)
{
	cJSON *copy NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!result || !value)
		return NB_ERR_INVAL;
	copy = cJSON_Duplicate(value, 1);
	if (!copy)
		return NB_ERR_NOMEM;
	return tool_result_field_take(result, key, &copy);
}

int tool_result_set_ui_copy(struct tool_result *result, const cJSON *ui)
{
	return tool_result_field_copy(result, "ui", ui);
}

int tool_result_set_meta_copy(struct tool_result *result, const cJSON *meta)
{
	return tool_result_field_copy(result, "meta", meta);
}

int tool_result_finalize(struct tool_result *result)
{
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *artifacts NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *meta NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	char *json NB_CLEANUP(cJSON_FreeTextOwner) = NULL;

	if (!result)
		return NB_ERR_INVAL;
	root = tool_result_copy_envelope(result);
	artifacts = tool_artifact_list_json_create(&result->artifacts);
	if (!root || !artifacts)
		return NB_ERR_NOMEM;
	cJSON_DeleteItemFromObjectCaseSensitive(root, "artifacts");
	if (!cJSON_AddItemToObjectCS(root, "artifacts", artifacts))
		return NB_ERR_INVAL;
	artifacts = NULL;
	if (!cJSON_GetObjectItemCaseSensitive(root, "ui") && !cJSON_AddNullToObject(root, "ui"))
		return NB_ERR_NOMEM;
	if (!cJSON_GetObjectItemCaseSensitive(root, "meta")) {
		meta = cJSON_CreateObject();
		if (!meta || !cJSON_AddItemToObjectCS(root, "meta", meta))
			return NB_ERR_NOMEM;
		meta = NULL;
	}
	const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
	const cJSON *item;

	if (cJSON_IsObject(data)) {
		cJSON_ArrayForEach (item, data) {
			if (!item->string || cJSON_GetObjectItem(root, item->string) ||
			    cJSON_IsArray(item) || cJSON_IsObject(item))
				continue;
			cJSON *copy NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Duplicate(item, 1);

			if (!copy || !cJSON_AddItemToObject(root, item->string, copy))
				return NB_ERR_NOMEM;
			copy = NULL;
		}
	}
	json = cJSON_PrintUnformatted(root);
	if (!json)
		return NB_ERR_NOMEM;
	tool_result_commit(result, &root, 1);
	result->owned = json;
	json = NULL;
	result->text.ptr = result->owned;
	result->text.len = strlen(result->owned);
	return 0;
}

int tool_result_add_artifact_copy(struct tool_result *result, enum tool_artifact_kind kind,
				  const char *path)
{
	struct tool_artifact *artifact;

	if (!result || !path || !*path)
		return NB_ERR_INVAL;
	if (result->artifacts.count >= TOOL_ARTIFACT_MAX)
		return NB_ERR_NO_SPACE;
	artifact = &result->artifacts.items[result->artifacts.count++];
	memset(artifact, 0, sizeof(*artifact));
	artifact->kind = kind;
	strncpy(artifact->path, path, sizeof(artifact->path) - 1);
	return 0;
}

int tool_result_add_image_copy(struct tool_result *result, const char *path, int width, int height)
{
	int rc = tool_result_add_artifact_copy(result, TOOL_ARTIFACT_IMAGE, path);

	if (rc != 0)
		return rc;
	struct tool_artifact *artifact = &result->artifacts.items[result->artifacts.count - 1];

	artifact->width = width;
	artifact->height = height;
	return 0;
}

int tool_result_add_video_copy(struct tool_result *result, const char *path, int duration_seconds)
{
	int rc = tool_result_add_artifact_copy(result, TOOL_ARTIFACT_VIDEO, path);

	if (rc != 0)
		return rc;
	struct tool_artifact *artifact = &result->artifacts.items[result->artifacts.count - 1];

	artifact->duration_seconds = duration_seconds;
	return 0;
}

static int tool_result_read_artifact(struct tool_result *result, const cJSON *item)
{
	const cJSON *kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
	const cJSON *path = cJSON_GetObjectItemCaseSensitive(item, "path");

	if (!cJSON_IsString(path))
		path = cJSON_GetObjectItemCaseSensitive(item, "output_path");
	if (!cJSON_IsString(path) || !path->valuestring)
		return 0;
	return tool_result_add_artifact_copy(
		result, tool_artifact_kind_from_string(cJSON_GetStringValue(kind)),
		path->valuestring);
}

int tool_result_set_artifacts_copy(struct tool_result *result, const cJSON *artifacts)
{
	struct tool_result parsed = {0};
	int rc = 0;

	if (!result || !artifacts)
		return NB_ERR_INVAL;
	if (cJSON_IsArray(artifacts)) {
		const cJSON *item;

		cJSON_ArrayForEach (item, artifacts) {
			rc = tool_result_read_artifact(&parsed, item);
			if (rc != 0)
				return rc;
		}
	} else if (cJSON_IsObject(artifacts)) {
		rc = tool_result_read_artifact(&parsed, artifacts);
		if (rc != 0)
			return rc;
	} else {
		return NB_ERR_INVAL;
	}
	result->artifacts = parsed.artifacts;
	return 0;
}

int tool_result_set_artifacts_take(struct tool_result *result, cJSON **artifacts)
{
	int rc;

	if (!result || !artifacts || artifacts == &result->envelope || artifacts == &result->data ||
	    artifacts == &result->ui || artifacts == &result->meta ||
	    tool_result_aliases(result, *artifacts))
		return NB_ERR_INVAL;
	rc = tool_result_set_artifacts_copy(result, *artifacts);
	if (rc == 0) {
		cJSON_Delete(*artifacts);
		*artifacts = NULL;
	}
	return rc;
}
