#ifndef MORPH_CLEANUP_H
#define MORPH_CLEANUP_H

#include <stdlib.h>

#if defined(__GNUC__) || defined(__clang__)
#define MORPH_CLEANUP(fn) __attribute__((__cleanup__(fn)))
#else
#error "morph requires compiler support for the cleanup attribute"
#endif

/* Pointer owners must be initialized to NULL before automatic cleanup. */
static inline void morph_charp_cleanup(char **owner)
{
	if (!owner)
		return;
	free(*owner);
	*owner = NULL;
}

/* Release a local owner without freeing the object. This cannot fail. */
static inline char *morph_charp_steal(char **owner)
{
	char *value;

	if (!owner)
		return NULL;
	value = *owner;
	*owner = NULL;
	return value;
}

#endif
