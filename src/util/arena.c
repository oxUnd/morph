#include "arena.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_ALIGN sizeof(void *)
#define ARENA_DEFAULT_SIZE (64 * 1024)
#define ARENA_LARGE_RATIO 2

static struct arena *arena_new_region(size_t cap)
{
	struct arena *a = malloc(sizeof(*a));
	if (!a)
		return NULL;
	a->buf = malloc(cap);
	if (!a->buf) {
		free(a);
		return NULL;
	}
	a->cap = cap;
	a->used = 0;
	a->next = NULL;
	a->large = NULL;
	a->cleanup = NULL;
	return a;
}

struct arena *arena_create(size_t cap)
{
	if (cap == 0)
		cap = ARENA_DEFAULT_SIZE;
	return arena_new_region(cap);
}

static void arena_free_large(struct arena *a)
{
	struct arena_large *large;

	if (!a)
		return;

	large = a->large;
	while (large) {
		struct arena_large *next = large->next;

		free(large->ptr);
		free(large);
		large = next;
	}
	a->large = NULL;
}

void arena_cleanup_run(struct arena *a)
{
	struct arena_cleanup *cleanup;

	if (!a)
		return;

	cleanup = a->cleanup;
	while (cleanup) {
		struct arena_cleanup *next = cleanup->next;

		if (cleanup->handler)
			cleanup->handler(cleanup->data);
		cleanup = next;
	}
	a->cleanup = NULL;
}

void arena_destroy(struct arena *a)
{
	struct arena *cur = a;

	arena_cleanup_run(a);
	arena_free_large(a);
	while (cur) {
		struct arena *next = cur->next;
		free(cur->buf);
		free(cur);
		cur = next;
	}
}

static size_t align_up(size_t v, size_t align)
{
	return (v + align - 1) & ~(align - 1);
}

static void *arena_alloc_large(struct arena *a, size_t size, size_t align)
{
	struct arena_large *large;
	void *ptr;
	size_t total;
	uintptr_t aligned;

	if (align == 0)
		align = ARENA_ALIGN;

	if (size > SIZE_MAX - align + 1)
		return NULL;
	total = size + align - 1;
	ptr = malloc(total);
	if (!ptr)
		return NULL;

	large = malloc(sizeof(*large));
	if (!large) {
		free(ptr);
		return NULL;
	}

	aligned = (uintptr_t)ptr;
	aligned = (uintptr_t)align_up((size_t)aligned, align);
	large->ptr = ptr;
	large->next = a->large;
	a->large = large;
	memset((void *)aligned, 0, size);
	return (void *)aligned;
}

void *arena_alloc_aligned(struct arena *a, size_t size, size_t align)
{
	if (!a || size == 0)
		return NULL;

	if (align == 0)
		align = ARENA_ALIGN;

	size_t aligned_used = align_up(a->used, align);
	size_t new_used = aligned_used + size;

	if (new_used <= a->cap) {
		void *ptr = a->buf + aligned_used;
		a->used = new_used;
		memset(ptr, 0, size);
		return ptr;
	}

	if (size + align > a->cap) {
		return arena_alloc_large(a, size, align);
	}

	if (size > a->cap / ARENA_LARGE_RATIO) {
		return arena_alloc_large(a, size, align);
	}

	struct arena *region = arena_new_region(a->cap);
	if (!region)
		return NULL;
	region->next = a->next;
	a->next = region;

	aligned_used = align_up(0, align);
	void *ptr = region->buf + aligned_used;
	region->used = aligned_used + size;
	memset(ptr, 0, size);
	return ptr;
}

void *arena_alloc(struct arena *a, size_t size)
{
	return arena_alloc_aligned(a, size, ARENA_ALIGN);
}

void arena_reset(struct arena *a)
{
	struct arena *cur;

	if (!a)
		return;
	arena_cleanup_run(a);
	arena_free_large(a);
	cur = a->next;
	while (cur) {
		struct arena *next = cur->next;
		free(cur->buf);
		free(cur);
		cur = next;
	}
	a->next = NULL;
	a->used = 0;
}

char *arena_strdup(struct arena *a, const char *s)
{
	if (!s)
		return NULL;
	size_t len = strlen(s) + 1;
	char *dst = arena_alloc(a, len);
	if (dst)
		memcpy(dst, s, len);
	return dst;
}

struct arena_cleanup *arena_cleanup_add(struct arena *a, size_t size)
{
	struct arena_cleanup *cleanup;

	if (!a)
		return NULL;

	cleanup = arena_alloc(a, sizeof(*cleanup));
	if (!cleanup)
		return NULL;
	if (size > 0) {
		cleanup->data = arena_alloc(a, size);
		if (!cleanup->data)
			return NULL;
	}
	cleanup->next = a->cleanup;
	a->cleanup = cleanup;
	return cleanup;
}
