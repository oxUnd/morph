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
	struct json *envelope;
	morph_str_t text;
	char *owned;
	struct json *data;
	struct json *ui;
	struct json *meta;
	struct tool_artifact_list artifacts;
};

void tool_result_init(struct tool_result *result);
void tool_result_cleanup(struct tool_result *result);
void tool_result_clear(struct tool_result *result);
/* With a valid result, consumes detached data even on construction failure.
 * NULL data creates an empty object. Borrowed, reused or ancestor aliases are
 * rejected without transfer; a NULL result also leaves ownership unchanged. */
int tool_result_success(struct tool_result *result, struct json *data);
/* Copies borrowed text before replacing the result; NULL result is rejected. */
int tool_result_success_text(struct tool_result *result, const char *text);
int tool_result_successf(struct tool_result *result, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
/* Consumes malloc-owned text on every path, including a NULL result. */
int tool_result_success_json_text(struct tool_result *result, char *json);
/* Copies borrowed code/message before replacement. Construction failure keeps
 * the original result intact, including when text aliases its JSON fields. */
int tool_result_error(struct tool_result *result, const char *code, const char *message);
int tool_result_errorf(struct tool_result *result, const char *code, const char *fmt, ...);
/* Consume detached, unique input for a valid result, including on allocation
 * failure. Ownership errors leave both trees unchanged; reusing the current
 * field itself is a no-op. NULL result leaves ownership unchanged. */
int tool_result_set_ui(struct tool_result *result, struct json *ui);
int tool_result_set_meta(struct tool_result *result, struct json *meta);
int tool_result_finalize(struct tool_result *result);
int tool_result_add_artifact(struct tool_result *result, enum tool_artifact_kind kind,
			     const char *path);
int tool_result_add_image(struct tool_result *result, const char *path, int width, int height);
int tool_result_add_video(struct tool_result *result, const char *path, int duration_seconds);
/* Consumes artifacts when result is valid; a NULL result leaves ownership unchanged. */
int tool_result_take_artifacts(struct tool_result *result, struct json *artifacts);

#ifdef __cplusplus
}
#endif

#endif
