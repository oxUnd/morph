#ifndef NB_VEC_H
#define NB_VEC_H

#include "nb/base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A vector storing elements by value. The vector owns its storage but not
 * any resources referenced by the elements. */
typedef struct nb_vec {
    unsigned char *data;
    size_t len;
    size_t cap;
    size_t elem_size;
} nb_vec;

NB_EXPORT void nb_vec_init(nb_vec *vec, size_t elem_size);
NB_EXPORT void nb_vec_destroy(nb_vec *vec);
/* Exchanges storage without allocating; element resources remain with their owner. */
NB_EXPORT void nb_vec_move(nb_vec *dst, nb_vec *src);
NB_EXPORT bool nb_vec_reserve(nb_vec *vec, size_t count);
/* Releases unused tail storage. An empty vector releases all storage. Returns
 * false when the backing allocation cannot be resized; the vector stays
 * usable with its previous capacity. */
NB_EXPORT bool nb_vec_shrink(nb_vec *vec);

/* Returns a pointer to an uninitialized new element, or NULL on allocation
 * failure. */
NB_EXPORT void *nb_vec_push(nb_vec *vec);
/* Appends count uninitialized slots atomically, or leaves the vector unchanged. */
NB_EXPORT void *nb_vec_push_n(nb_vec *vec, size_t count);
/* elem may point to an existing element, including when growth is needed. */
NB_EXPORT bool nb_vec_push_copy(nb_vec *vec, const void *elem);

/* Inserts a copy of elem at index, shifting later elements to the right.
 * index == len appends. Returns false for a NULL elem, index > len, or growth
 * failure; len is unchanged on failure. elem may point to an existing element. */
NB_EXPORT bool nb_vec_insert(nb_vec *vec, size_t index, const void *elem);

/* Removes the element at index, shifting later elements to the left. Does
 * nothing when index >= len. */
NB_EXPORT void nb_vec_remove(nb_vec *vec, size_t index);
/* Removes up to count elements starting at index, without owning their fields. */
NB_EXPORT void nb_vec_remove_range(nb_vec *vec, size_t index, size_t count);

NB_EXPORT void *nb_vec_at(const nb_vec *vec, size_t index);
NB_EXPORT void *nb_vec_at_mut(nb_vec *vec, size_t index);
NB_EXPORT void nb_vec_pop(nb_vec *vec);
NB_EXPORT void nb_vec_clear(nb_vec *vec);
NB_EXPORT size_t nb_vec_len(const nb_vec *vec);
NB_EXPORT bool nb_vec_empty(const nb_vec *vec);
NB_EXPORT size_t nb_vec_elem_size(const nb_vec *vec);

#ifdef __cplusplus
}
#endif

#endif /* NB_VEC_H */
