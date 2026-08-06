#ifndef AGENT_HISTORY_H
#define AGENT_HISTORY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "models/llm.h"
#include "agent/tool.h"
#include "session.h"
#include "util/array.h"

struct arena;
struct react_context;
struct tokenizer;

struct agent_history_diagnostic {
	int active_items;
	int dangling_calls;
	int orphan_results;
	int invalid_payloads;
	int token_mismatches;
};

int agent_history_build_chat_messages(const struct model_history_item *items,
				      morph_array_t *messages,
				      struct arena *arena);
int agent_history_record_user(struct react_context *ctx, const char *content);
int agent_history_record_user_steer(struct react_context *ctx,
				    const char *content, int sequence);
int agent_history_record_assistant(struct react_context *ctx,
				   const char *content);
int agent_history_record_tool_calls(struct react_context *ctx,
				    const char *content,
				    const char *reasoning_content,
				    const struct tool_call *calls,
				    int call_count);
int agent_history_normalize_tool_arguments(const char *arguments,
					   char **normalized);
int agent_history_record_tool_result(struct react_context *ctx,
				     const char *tool_call_id,
				     const char *provider_call_id,
				     const char *tool_name,
				     const char *content,
				     int error_code);
int agent_history_record_tool_result_ex(struct react_context *ctx,
	const char *tool_call_id, const char *provider_call_id,
	const char *tool_name, const char *content, int error_code,
	const struct tool_artifact_list *artifacts, const cJSON *tool_meta);
int agent_history_prepare_tool_content(struct react_context *ctx,
	const char *content, char **prepared, int *truncated);
int agent_history_maybe_compact(struct react_context *ctx);
int agent_history_compact(struct react_context *ctx, int force);
int agent_history_repair_interrupted(struct db *db, int64_t session_id);
int agent_history_diagnose(const struct model_history_item *items,
	struct tokenizer *tokenizer, struct agent_history_diagnostic *diagnostic);
int agent_history_repair(struct db *db, int64_t session_id,
	struct tokenizer *tokenizer,
	struct agent_history_diagnostic *before, int *changed);
int agent_history_record_receipt(struct react_context *ctx,
	const char *name, const char *phase, const char *message,
	const char *task, int count, int error_code);

#ifdef __cplusplus
}
#endif

#endif
