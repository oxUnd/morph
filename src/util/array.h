#ifndef MORPH_ARRAY_H
#define MORPH_ARRAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include "error.h"

struct arena;

#define MORPH_ARRAY_INIT_CAP  16

typedef struct {
	void   *elts;
	size_t  nelts;
	size_t  cap;
	size_t  size;
	int     heap_alloc;
} morph_array_t;

int  morph_array_init(morph_array_t *a, size_t init_cap, size_t elem_size);
int  morph_array_init_arena(morph_array_t *a, struct arena *arena,
                            size_t init_cap, size_t elem_size);
void morph_array_cleanup(morph_array_t *a);

void *morph_array_push(morph_array_t *a);
void *morph_array_push_n(morph_array_t *a, size_t n);
void  morph_array_pop(morph_array_t *a);

void *morph_array_get(const morph_array_t *a, size_t i);

int   morph_array_reserve(morph_array_t *a, size_t min_cap);
void  morph_array_clear(morph_array_t *a);

#define morph_array_foreach(ptr, a, type)                          \
	for (size_t _i_ = 0;                                           \
	     _i_ < (a)->nelts && ((ptr) = (type *)(a)->elts + _i_);   \
	     _i_++)

#ifdef __cplusplus
}
#endif

#endif
