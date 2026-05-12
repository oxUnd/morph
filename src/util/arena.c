#include "arena.h"
#include <stdlib.h>
#include <string.h>

#define ARENA_ALIGN sizeof(void *)
#define ARENA_DEFAULT_SIZE (64 * 1024)

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
	return a;
}

struct arena *arena_create(size_t cap)
{
	if (cap == 0)
		cap = ARENA_DEFAULT_SIZE;
	return arena_new_region(cap);
}

void arena_destroy(struct arena *a)
{
	struct arena *cur = a;
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

void *arena_alloc_aligned(struct arena *a, size_t size, size_t align)
{
	if (!a || size == 0)
		return NULL;

	size_t aligned_used = align_up(a->used, align);
	size_t new_used = aligned_used + size;

	if (new_used <= a->cap) {
		void *ptr = a->buf + aligned_used;
		a->used = new_used;
		memset(ptr, 0, size);
		return ptr;
	}

	if (size + align > a->cap) {
		struct arena *region = arena_new_region(align_up(size, ARENA_ALIGN) + align);
		if (!region)
			return NULL;
		size_t region_aligned = align_up(0, align);
		void *ptr = region->buf + region_aligned;
		region->used = region_aligned + size;
		region->next = a->next;
		a->next = region;
		memset(ptr, 0, size);
		return ptr;
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
	if (!a)
		return;
	struct arena *cur = a->next;
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