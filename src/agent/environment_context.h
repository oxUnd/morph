#ifndef MORPH_ENVIRONMENT_CONTEXT_H
#define MORPH_ENVIRONMENT_CONTEXT_H

#include "prompt_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* All collected strings belong to the caller's turn arena. */
struct environment_context {
	const char *cwd;
	const char *shell;
	const char *os;
	const char *arch;
	const char *current_date;
	const char *timezone;
	const char *git_branch;
	const char *git_root;
	const char *sandbox;
};

int environment_context_collect(struct environment_context *out,
	const struct prompt_context_input *input, struct arena *arena);
/* Pure serialization; does not read environment variables or the OS. */
int environment_context_render(const struct environment_context *ctx,
	morph_buf_t *out);

#ifdef __cplusplus
}
#endif
#endif
