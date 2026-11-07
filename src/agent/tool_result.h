#ifndef MORPH_TOOL_RESULT_H
#define MORPH_TOOL_RESULT_H

#include "agent/tool_artifact.h"
#include "util/str.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tool_runtime_context;

struct tool_result {
	/* Borrowed execution input, valid until the tool call returns. */
	const struct tool_runtime_context *runtime;
	cJSON *envelope;
	morph_str_t text;
	char *owned;
	cJSON *data;
	cJSON *ui;
	cJSON *meta;
	struct tool_artifact_list artifacts;
};

void tool_result_init(struct tool_result *result);
void tool_result_cleanup(struct tool_result *result);
void tool_result_clear(struct tool_result *result);
/* Copy setters borrow their inputs for this call and store independent data. Take setters require
 * independent, detached owners: success clears the source; failure preserves both owners. */
int tool_result_success_copy(struct tool_result *result, const cJSON *data);
int tool_result_success_take(struct tool_result *result, cJSON **data);
int tool_result_success_text_copy(struct tool_result *result, const char *text);
int tool_result_success_format(struct tool_result *result, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
int tool_result_success_json_text_copy(struct tool_result *result, const char *json);
/* The text owner must use malloc/free, not a third-party allocator. */
int tool_result_success_json_text_take(struct tool_result *result, char **json);
int tool_result_error_copy(struct tool_result *result, const char *code, const char *message);
int tool_result_error_format(struct tool_result *result, const char *code, const char *fmt, ...);
int tool_result_set_ui_copy(struct tool_result *result, const cJSON *ui);
int tool_result_set_ui_take(struct tool_result *result, cJSON **ui);
int tool_result_set_meta_copy(struct tool_result *result, const cJSON *meta);
int tool_result_set_meta_take(struct tool_result *result, cJSON **meta);
int tool_result_finalize(struct tool_result *result);
int tool_result_add_artifact_copy(struct tool_result *result, enum tool_artifact_kind kind,
				  const char *path);
int tool_result_add_image_copy(struct tool_result *result, const char *path, int width, int height);
int tool_result_add_video_copy(struct tool_result *result, const char *path, int duration_seconds);
int tool_result_set_artifacts_copy(struct tool_result *result, const cJSON *artifacts);
int tool_result_set_artifacts_take(struct tool_result *result, cJSON **artifacts);

#ifdef __cplusplus
}
#endif

#endif
