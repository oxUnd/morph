#ifndef ARENA_H
#define ARENA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

struct arena {
	char *buf;
	size_t cap;
	size_t used;
	struct arena *next;
};

struct arena *arena_create(size_t cap);
void arena_destroy(struct arena *a);
void *arena_alloc(struct arena *a, size_t size);
void *arena_alloc_aligned(struct arena *a, size_t size, size_t align);
void arena_reset(struct arena *a);
char *arena_strdup(struct arena *a, const char *s);

#ifdef __cplusplus
}
#endif

#endif