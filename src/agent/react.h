#ifndef REACT_H
#define REACT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <signal.h>
#include "agent/tool.h"
#include "agent/context.h"
#include "skill/skill.h"

enum react_step_type {
	REACT_STEP_THOUGHT,
	REACT_STEP_ACTION,
	REACT_STEP_OBSERVATION,
	REACT_STEP_REFLECTION,
	REACT_STEP_FINAL,
};

enum react_state {
	REACT_STATE_INIT,
	REACT_STATE_THINKING,
	REACT_STATE_ACTING,
	REACT_STATE_OBSERVING,
	REACT_STATE_GUARDRAIL,
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
	char *tool_call_id;
	struct react_step *next;
};

enum guardrail_verdict {
	GUARDRAIL_PASS,
	GUARDRAIL_FAIL_NO_TOOLS,
	GUARDRAIL_FAIL_NO_OUTPUT,
	GUARDRAIL_FAIL_EMPTY_ANSWER,
	GUARDRAIL_FAIL_CONSECUTIVE_EMPTY,
};

struct guardrail_result {
	enum guardrail_verdict verdict;
	char reason[512];
};

struct guardrail_config {
	int enabled;
	int max_retries;
	int min_tool_calls;
	int must_have_output;
	int max_empty_rounds;
};

struct react_context {
	struct react_step *steps;
	int step_count;
	int max_iterations;
	int step_timeout_seconds;
	int tool_max_retries;
	struct guardrail_config guardrail;
	int guardrail_retry_count;
	struct tool_registry *tools;
	struct message_list *messages;
	struct tokenizer *tokenizer;
	struct compress_config compress;
	void *llm_model;
	char *final_answer;
	enum react_state state;
	char tool_fail_name[64];
	char tool_fail_args[512];
	int tool_fail_count;
	int empty_round_count;
	volatile sig_atomic_t cancelled;
	struct arena *arena;
	char *system_prompt;
	struct skill_registry *skills;
};

typedef int (*react_output_cb)(enum react_step_type type,
			       const char *content, void *user_data);

struct react_context *react_context_create(struct tool_registry *tools,
					   struct tokenizer *tok,
					   struct compress_config *cfg,
					   struct guardrail_config *gcfg);
void react_context_destroy(struct react_context *ctx);
void react_reset(struct react_context *ctx);

int react_run(struct react_context *ctx, const char *user_input,
	      react_output_cb cb, void *user_data);
void react_cancel(struct react_context *ctx);
extern volatile sig_atomic_t react_sigint_flag;

struct react_step *react_step_create(struct arena *arena,
				     enum react_step_type type,
				     const char *content,
				     const char *tool_name,
				     const char *tool_args,
				     const char *tool_call_id);

void react_step_destroy(struct react_step *step);

const char *react_step_type_name(enum react_step_type type);
const char *react_state_name(enum react_state state);

#ifdef __cplusplus
}
#endif

#endif
