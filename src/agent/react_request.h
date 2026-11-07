#ifndef MORPH_REACT_REQUEST_H
#define MORPH_REACT_REQUEST_H

#include "react.h"
#include "models/llm.h"
#include "util/array.h"

#ifdef __cplusplus
extern "C" {
#endif

int react_count_input_text(const char *text, void *user_data);

/* Request owners keep copied model input alive through the complete call. */
struct react_request_messages {
	char *system_prompt;
	morph_array_t messages;
};
void react_request_messages_cleanup(struct react_request_messages *request);
void react_tool_descriptors_cleanup(morph_array_t *descriptors);
char *react_system_prompt_create(struct react_context *ctx);
int react_prepare_active_tools(struct react_context *ctx, morph_array_t *storage,
			       struct tool_desc **tools, int *count);
int react_request_messages_create(struct react_context *ctx, const struct model *llm,
				  const char *system_prompt, const struct chat_message *messages,
				  int msg_count, struct react_request_messages *out);
int react_maybe_compact_active_window(struct react_context *ctx, struct model *llm,
				      const char *system_prompt, morph_array_t *messages,
				      const struct tool_desc *tools, int tool_count, int iteration);
int react_messages_create(struct react_context *ctx, morph_array_t *messages,
			  const char *current_user_input);
int react_checkpoint_create(const char *text, void *user_data, char **out);

#ifdef __cplusplus
}
#endif

#endif
