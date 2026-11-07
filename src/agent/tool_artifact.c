#include "tool_artifact.h"

#include <string.h>

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
		cJSON_AddStringToObject(obj, "kind", tool_artifact_kind_name(artifact->kind));
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
			cJSON_AddNumberToObject(obj, "size_bytes", (double)artifact->size_bytes);
		cJSON_AddItemToArray(arr, obj);
	}
	return arr;
}
