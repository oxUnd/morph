#ifndef MORPH_PROMPT_CONTEXT_H
#define MORPH_PROMPT_CONTEXT_H

#include <nb/buf.h>
#include <nb/cleanup.h>

struct environment_context;

#ifdef __cplusplus
extern "C" {
#endif

/* Borrowed execution settings. NULL means unknown, not unrestricted. */
struct prompt_context_input {
	const char *cwd;
	const char *shell;
	const char *sandbox;
};

/* Providers render borrowed snapshots in declaration order, outside history. */
struct prompt_context_provider {
	const char *name;
	int (*render)(const void *snapshot, nb_buf *out);
	const void *snapshot;
};

int prompt_context_create(const struct prompt_context_provider *providers, size_t count,
			  char **out);
char *prompt_reference_create(const char *content);

int prompt_context_default_create(const struct environment_context *environment, char **out);

#ifdef __cplusplus
}
#endif
#endif
