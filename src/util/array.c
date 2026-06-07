#include "array.h"
#include "arena.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int array_bytes(size_t cap, size_t elem_size, size_t *bytes)
{
	if (elem_size != 0 && cap > SIZE_MAX / elem_size)
		MORPH_RETURN(-EOVERFLOW);
	*bytes = cap * elem_size;
	return 0;
}

int morph_array_init(morph_array_t *a, size_t init_cap, size_t elem_size)
{
	size_t bytes;
	int rc;

	if (!a || elem_size == 0)
		MORPH_RETURN(-EINVAL);
	if (init_cap == 0)
		init_cap = MORPH_ARRAY_INIT_CAP;
	rc = array_bytes(init_cap, elem_size, &bytes);
	if (rc != 0)
		return rc;
	a->elts = malloc(bytes);
	if (!a->elts)
		MORPH_RETURN(-ENOMEM);
	a->nelts = 0;
	a->cap = init_cap;
	a->size = elem_size;
	a->heap_alloc = 1;
	return 0;
}

int morph_array_init_arena(morph_array_t *a, struct arena *arena,
                           size_t init_cap, size_t elem_size)
{
	size_t bytes;
	int rc;

	if (!a || !arena || elem_size == 0)
		MORPH_RETURN(-EINVAL);
	if (init_cap == 0)
		init_cap = MORPH_ARRAY_INIT_CAP;
	rc = array_bytes(init_cap, elem_size, &bytes);
	if (rc != 0)
		return rc;
	a->elts = arena_alloc(arena, bytes);
	if (!a->elts)
		MORPH_RETURN(-ENOMEM);
	a->nelts = 0;
	a->cap = init_cap;
	a->size = elem_size;
	a->heap_alloc = 0;
	return 0;
}

void morph_array_cleanup(morph_array_t *a)
{
	if (!a)
		return;
	if (a->heap_alloc)
		free(a->elts);
	a->elts = NULL;
	a->nelts = 0;
	a->cap = 0;
	a->size = 0;
	a->heap_alloc = 0;
}

static int array_grow(morph_array_t *a, size_t min_cap)
{
	size_t new_cap;
	size_t bytes;
	void *new_elts;

	if (min_cap <= a->cap)
		return 0;
	new_cap = a->cap ? a->cap : MORPH_ARRAY_INIT_CAP;
	while (new_cap < min_cap) {
		size_t next = new_cap + new_cap / 2 + 8;
		if (next <= new_cap)
			MORPH_RETURN(-EOVERFLOW);
		new_cap = next;
	}
	if (array_bytes(new_cap, a->size, &bytes) != 0)
		MORPH_RETURN(-EOVERFLOW);
	if (a->heap_alloc) {
		new_elts = realloc(a->elts, bytes);
		if (!new_elts)
			MORPH_RETURN(-ENOMEM);
	} else {
		MORPH_RETURN(-ENOMEM);
	}
	a->elts = new_elts;
	a->cap = new_cap;
	return 0;
}

void *morph_array_push(morph_array_t *a)
{
	if (!a)
		return NULL;
	if (a->nelts >= a->cap) {
		if (a->cap == SIZE_MAX)
			return NULL;
		if (array_grow(a, a->cap + 1) != 0)
			return NULL;
	}
	{
		void *slot = (char *)a->elts + a->nelts * a->size;
		a->nelts++;
		return slot;
	}
}

void *morph_array_push_n(morph_array_t *a, size_t n)
{
	void *slot;

	if (!a || n == 0)
		return NULL;
	if (n > SIZE_MAX - a->nelts)
		return NULL;
	if (a->nelts + n > a->cap) {
		if (array_grow(a, a->nelts + n) != 0)
			return NULL;
	}
	slot = (char *)a->elts + a->nelts * a->size;
	a->nelts += n;
	return slot;
}

void morph_array_pop(morph_array_t *a)
{
	if (!a || a->nelts == 0)
		return;
	a->nelts--;
}

void *morph_array_get(const morph_array_t *a, size_t i)
{
	if (!a || i >= a->nelts)
		return NULL;
	return (char *)a->elts + i * a->size;
}

int morph_array_reserve(morph_array_t *a, size_t min_cap)
{
	if (!a)
		MORPH_RETURN(-EINVAL);
	return array_grow(a, min_cap);
}

void morph_array_clear(morph_array_t *a)
{
	if (!a)
		return;
	a->nelts = 0;
}
