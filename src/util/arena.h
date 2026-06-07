#ifndef ARENA_H
#define ARENA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

typedef void (*arena_cleanup_fn)(void *data);

struct arena_cleanup {
	arena_cleanup_fn handler;
	void *data;
	struct arena_cleanup *next;
};

struct arena_large {
	void *ptr;
	struct arena_large *next;
};

struct arena {
	char *buf;
	size_t cap;
	size_t used;
	int failed;
	struct arena *next;
	struct arena *current;
	struct arena_large *large;
	struct arena_cleanup *cleanup;
};

struct arena *arena_create(size_t cap);
void arena_destroy(struct arena *a);
void *arena_alloc(struct arena *a, size_t size);
void *arena_alloc_aligned(struct arena *a, size_t size, size_t align);
void arena_reset(struct arena *a);
char *arena_strdup(struct arena *a, const char *s);
void arena_cleanup_run(struct arena *a);
struct arena_cleanup *arena_cleanup_add(struct arena *a, size_t size);

#ifdef __cplusplus
}
#endif

#endif
