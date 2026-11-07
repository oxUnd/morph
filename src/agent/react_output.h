#ifndef MORPH_REACT_OUTPUT_H
#define MORPH_REACT_OUTPUT_H

#include "agent/tool_artifact.h"

#ifdef __cplusplus
extern "C" {
#endif

enum react_step_type {
	REACT_STEP_THOUGHT,
	REACT_STEP_ACTION,
	REACT_STEP_OBSERVATION,
	REACT_STEP_REFLECTION,
	REACT_STEP_FINAL,
	REACT_STEP_REASONING,
	REACT_STEP_CONTENT,
};

enum react_output_status {
	REACT_OUTPUT_NONE,
	REACT_OUTPUT_DELTA,
	REACT_OUTPUT_STARTED,
	REACT_OUTPUT_COMPLETED,
	REACT_OUTPUT_FAILED,
	REACT_OUTPUT_CANCELLED,
	REACT_OUTPUT_TIMEOUT,
};

struct react_output_event {
	enum react_step_type type;
	enum react_output_status status;
	const char *text;
	const char *tool_name;
	const char *tool_args;
	const char *tool_call_id;
	int error_code;
	const struct tool_artifact_list *artifacts;
	const struct json *data;
	const struct json *ui;
};

typedef int (*react_output_cb)(const struct react_output_event *event, void *user_data);

/* Event fields are borrowed until the synchronous callback returns. */
int react_output_emit(react_output_cb callback, void *user_data,
		      const struct react_output_event *event);

#ifdef __cplusplus
}
#endif

#endif
