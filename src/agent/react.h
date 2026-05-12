#ifndef REACT_H
#define REACT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include "agent/context.h"

enum react_step_type {
	REACT_STEP_THOUGHT,
	REACT_STEP_ACTION,
	REACT_STEP_OBSERVATION,
	REACT_STEP_FINAL,
};

enum react_state {
	REACT_STATE_INIT,
	REACT_STATE_THINKING,
	REACT_STATE_ACTING,
	REACT_STATE_OBSERVING,
	REACT_STATE_FINAL,
	REACT_STATE_DONE,
	REACT_STATE_ABORT,
	REACT_STATE_TOOL_FAIL,
};

struct react_step {
	enum react_step_type type;
	char *content;
	char *tool_name;
	char *tool_args;
	struct react_step *next;
};

struct react_context {
	struct react_step *steps;
	int step_count;
	int max_iterations;
	int step_timeout_seconds;
	int tool_max_retries;
	struct tool_registry *tools;
	struct message_list *messages;
	struct tokenizer *tokenizer;
	struct compress_config compress;
	void *llm_model;
	char *final_answer;
	enum react_state state;
	char tool_fail_name[64];
	int tool_fail_count;
};

typedef int (*react_output_cb)(enum react_step_type type,
			       const char *content, void *user_data);

struct react_context *react_context_create(struct tool_registry *tools,
					   struct tokenizer *tok,
					   struct compress_config *cfg);
void react_context_destroy(struct react_context *ctx);
void react_reset(struct react_context *ctx);

int react_run(struct react_context *ctx, const char *user_input,
	      react_output_cb cb, void *user_data);

struct react_step *react_step_create(enum react_step_type type,
				     const char *content,
				     const char *tool_name,
				     const char *tool_args);
void react_step_destroy(struct react_step *step);

const char *react_step_type_name(enum react_step_type type);
const char *react_state_name(enum react_state state);

#ifdef __cplusplus
}
#endif

#endif