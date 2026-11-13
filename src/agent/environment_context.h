#ifndef MORPH_ENVIRONMENT_CONTEXT_H
#define MORPH_ENVIRONMENT_CONTEXT_H

#include "prompt_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Collected strings are owned until environment_context_cleanup(). */
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

int environment_context_create(struct environment_context *out,
			       const struct prompt_context_input *input);
void environment_context_cleanup(struct environment_context *ctx);
/* Pure serialization; does not read environment variables or the OS. */
int environment_context_render(const struct environment_context *ctx, nb_buf *out);

#ifdef __cplusplus
}
#endif
#endif
