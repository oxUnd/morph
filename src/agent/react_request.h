#ifndef MORPH_REACT_REQUEST_H
#define MORPH_REACT_REQUEST_H

#include "react.h"
#include "models/llm.h"
#include "util/array.h"

int react_count_input_text(const char *text, void *user_data);

/* Private request builders. The caller owns iteration/message arenas. */
char *react_build_system_prompt(struct react_context *ctx, struct arena *arena);
int react_prepare_active_tools(struct react_context *ctx, morph_array_t *storage,
			       struct tool_desc **tools, int *count);
int react_prepare_request_messages(struct react_context *ctx, const struct model *llm,
				   struct arena *arena, const char **system_prompt,
				   struct chat_message **messages, int *msg_count);
int react_maybe_compact_active_window(struct react_context *ctx, struct model *llm,
				      const char *system_prompt, morph_array_t *messages,
				      const struct tool_desc *tools, int tool_count, int iteration);
int react_prepare_messages(struct react_context *ctx, morph_array_t *messages,
			   const char *current_user_input);
int react_summarize_checkpoint(const char *text, void *user_data, char **out);

#endif
