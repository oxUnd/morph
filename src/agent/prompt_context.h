#ifndef MORPH_PROMPT_CONTEXT_H
#define MORPH_PROMPT_CONTEXT_H

#include "util/cleanup.h"
#include "util/buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Borrowed execution settings. NULL means unknown, not unrestricted. */
struct prompt_context_input {
	const char *cwd;
	const char *shell;
	const char *sandbox;
};

/* Providers run once per turn in declaration order, outside history. */
struct prompt_context_provider {
	const char *name;
	int (*collect_render)(const struct prompt_context_input *input, morph_buf_t *out);
};

int prompt_context_create(const struct prompt_context_provider *providers, size_t count,
			  const struct prompt_context_input *input, char **out);
char *prompt_reference_create(const char *content);

int prompt_context_default_create(const struct prompt_context_input *input, char **out);

#ifdef __cplusplus
}
#endif
#endif
