#ifndef GUARDRAIL_H
#define GUARDRAIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "util/arena.h"
#include <stddef.h>

enum guardrail_verdict {
	GUARDRAIL_PASS = 0,
	GUARDRAIL_FAIL = 1,
};

enum guardrail_hook {
	GUARDRAIL_HOOK_INPUT,
	GUARDRAIL_HOOK_TOOL_OUTPUT,
	GUARDRAIL_HOOK_OUTPUT,
};

#define GUARDRAIL_RULES_MAX          16
#define GUARDRAIL_REASON_MAX         512
#define GUARDRAIL_ACTION_MAX         512
#define GUARDRAIL_NAME_MAX           64
#define GUARDRAIL_DESC_MAX           1024
#define GUARDRAIL_EXT_ENTRY_MAX      512

struct react_step;

struct guardrail_eval_ctx {
	const char *user_input;
	const char *tool_name;
	const char *tool_args;
	const char *tool_result;
	const char *proposed_answer;
	const void *steps;
	int empty_round_count;
	struct arena *arena;
};

typedef enum guardrail_verdict (*guardrail_rule_fn)(
	const struct guardrail_eval_ctx *ctx,
	char *reason_out,
	size_t reason_cap);

enum guardrail_rule_type {
	GUARDRAIL_RULE_C,
	GUARDRAIL_RULE_LLM,
	GUARDRAIL_RULE_EXT,
};

enum guardrail_ext_type {
	GUARDRAIL_EXT_EXEC = 0,
	GUARDRAIL_EXT_SO   = 1,
};

typedef int (*guardrail_ext_check_fn)(const char *text,
				      const char *rule_name,
				      const char *description,
				      char **result_json);

struct guardrail_rule {
	char name[GUARDRAIL_NAME_MAX];
	enum guardrail_hook hook;
	enum guardrail_rule_type type;
	enum guardrail_ext_type ext_type;
	int enabled;
	guardrail_rule_fn check;
	void *dl_handle;
	guardrail_ext_check_fn ext_check;
	char description[GUARDRAIL_DESC_MAX];
	char ext_entry[GUARDRAIL_EXT_ENTRY_MAX];
	char action_text[GUARDRAIL_ACTION_MAX];
};

struct guardrail_result {
	enum guardrail_verdict verdict;
	char reason[GUARDRAIL_REASON_MAX];
	const struct guardrail_rule *triggered_rule;
};

struct model;

struct guardrail_config {
	int enabled;
	int max_retries;
	int max_empty_rounds;
	struct guardrail_rule rules[GUARDRAIL_RULES_MAX];
	int rule_count;
	struct model *llm;
};

int guardrail_rule_register(struct guardrail_config *cfg,
			     const char *name,
			     enum guardrail_hook hook,
			     enum guardrail_rule_type type,
			     guardrail_rule_fn check,
			     const char *description,
			     const char *ext_entry,
			     const char *action_text);

int guardrail_ext_so_load(struct guardrail_rule *rule);
void guardrail_ext_so_unload(struct guardrail_rule *rule);

int guardrail_rule_disable(struct guardrail_config *cfg, const char *name);
int guardrail_rule_enable(struct guardrail_config *cfg, const char *name);
struct guardrail_rule *guardrail_rule_lookup(struct guardrail_config *cfg,
					      const char *name);

void guardrail_register_builtin_rules(struct guardrail_config *cfg);

struct guardrail_result guardrail_run_hook(const struct guardrail_config *cfg,
					   enum guardrail_hook hook,
					   const struct guardrail_eval_ctx *eval);

void guardrail_set_llm(struct guardrail_config *cfg, struct model *llm);

#ifdef __cplusplus
}
#endif

#endif
