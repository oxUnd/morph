#ifndef MORPH_TOOL_USER_EVIDENCE_H
#define MORPH_TOOL_USER_EVIDENCE_H

struct tool_runtime_context;

/* The active model interprets intent. This boundary verifies that the supplied
 * quote belongs to the latest actual user event, in any language. */
int tool_user_evidence_token_copy(const struct tool_runtime_context *runtime, const char *quote,
				  char **out);

#endif
