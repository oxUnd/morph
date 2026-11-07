#ifndef MORPH_ARRAY_H
#define MORPH_ARRAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include "error.h"

#define MORPH_ARRAY_INIT_CAP  16

typedef struct {
	void   *elts;
	size_t  nelts;
	size_t  cap;
	size_t  size;
} morph_array_t;

int  morph_array_init(morph_array_t *a, size_t init_cap, size_t elem_size);
void morph_array_cleanup(morph_array_t *a);
/* Destination must be empty; failure preserves both containers. */
int morph_array_take(morph_array_t *destination, morph_array_t *source);

/* Appends zeroed slots and lends their addresses. Any later push/reserve may
 * invalidate all slot pointers; clear/pop end the removed slots' lifetime. */
void *morph_array_push_borrow(morph_array_t *a);
void *morph_array_push_n_borrow(morph_array_t *a, size_t n);
void  morph_array_pop(morph_array_t *a);

/* Borrows a slot until storage growth, removal, move, or cleanup. */
void *morph_array_borrow(const morph_array_t *a, size_t i);

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
