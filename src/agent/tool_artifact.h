#ifndef MORPH_TOOL_ARTIFACT_H
#define MORPH_TOOL_ARTIFACT_H

#include "cJSON.h"
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOOL_ARTIFACT_MAX 32
#define TOOL_ARTIFACT_LABEL_MAX 128
#define TOOL_ARTIFACT_MIME_MAX 128

enum tool_artifact_kind {
	TOOL_ARTIFACT_FILE,
	TOOL_ARTIFACT_IMAGE,
	TOOL_ARTIFACT_VIDEO,
};

struct tool_artifact {
	enum tool_artifact_kind kind;
	char path[PATH_MAX];
	char mime[TOOL_ARTIFACT_MIME_MAX];
	char label[TOOL_ARTIFACT_LABEL_MAX];
	int width;
	int height;
	int duration_seconds;
	long long size_bytes;
};

struct tool_artifact_list {
	struct tool_artifact items[TOOL_ARTIFACT_MAX];
	int count;
};

const char *tool_artifact_kind_name(enum tool_artifact_kind kind);
enum tool_artifact_kind tool_artifact_kind_from_string(const char *kind);
cJSON *tool_artifacts_json_create(const struct tool_artifact *items, int count);
cJSON *tool_artifact_list_json_create(const struct tool_artifact_list *artifacts);

#ifdef __cplusplus
}
#endif

#endif
