#include "array.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int array_bytes(size_t cap, size_t elem_size, size_t *bytes)
{
	if (elem_size != 0 && cap > SIZE_MAX / elem_size)
		return -EOVERFLOW;
	*bytes = cap * elem_size;
	return 0;
}

int morph_array_init(morph_array_t *a, size_t init_cap, size_t elem_size)
{
	size_t bytes;
	int rc;

	if (!a || elem_size == 0)
		return -EINVAL;
	if (init_cap == 0)
		init_cap = MORPH_ARRAY_INIT_CAP;
	rc = array_bytes(init_cap, elem_size, &bytes);
	if (rc != 0)
		return rc;
	a->elts = malloc(bytes);
	if (!a->elts)
		return -ENOMEM;
	a->nelts = 0;
	a->cap = init_cap;
	a->size = elem_size;
	return 0;
}

void morph_array_cleanup(morph_array_t *a)
{
	if (!a)
		return;
	free(a->elts);
	a->elts = NULL;
	a->nelts = 0;
	a->cap = 0;
	a->size = 0;
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
			return -EOVERFLOW;
		new_cap = next;
	}
	if (array_bytes(new_cap, a->size, &bytes) != 0)
		return -EOVERFLOW;
	new_elts = realloc(a->elts, bytes);
	if (!new_elts)
		return -ENOMEM;
	a->elts = new_elts;
	a->cap = new_cap;
	return 0;
}

int morph_array_take(morph_array_t *destination, morph_array_t *source)
{
	if (!destination || !source || destination == source)
		return -EINVAL;
	if (destination->elts || destination->nelts)
		return -EEXIST;
	*destination = *source;
	memset(source, 0, sizeof(*source));
	return 0;
}

void *morph_array_push_borrow(morph_array_t *a)
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

void *morph_array_push_n_borrow(morph_array_t *a, size_t n)
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

void *morph_array_borrow(const morph_array_t *a, size_t i)
{
	if (!a || i >= a->nelts)
		return NULL;
	return (char *)a->elts + i * a->size;
}

int morph_array_reserve(morph_array_t *a, size_t min_cap)
{
	if (!a)
		return -EINVAL;
	return array_grow(a, min_cap);
}

void morph_array_clear(morph_array_t *a)
{
	if (!a)
		return;
	a->nelts = 0;
}
