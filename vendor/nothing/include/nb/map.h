#ifndef NB_MAP_H
#define NB_MAP_H

#include "nb/base.h"
#include "nb/str.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * String-keyed hash map with insertion-order iteration.
 *
 * Keys are copied and owned by the map. Values are borrowed void pointers by
 * default; an explicitly installed value destructor owns bulk cleanup of
 * values still present at clear/free. Replaced/deleted values belong to the
 * caller regardless of whether a destructor is installed.
 */
typedef struct nb_map nb_map;
typedef struct nb_map_iter {
    const nb_map *map;
    const void *entry;
} nb_map_iter;

/* Optional destructor for values that are still present when the map is
 * cleared or freed. It is not called for nb_map_del() results or for values
 * replaced by nb_map_put(); those are returned to the caller. */
typedef void (*nb_map_value_dtor)(void *value, void *ctx);

NB_EXPORT nb_map *nb_map_new(void);
NB_EXPORT nb_map *nb_map_new_with_cap(size_t cap);
NB_EXPORT void nb_map_free(nb_map *map);

/* Inserts or updates a key. On success *old_value (when old_value is not
 * NULL) receives the replaced value, or NULL for a new key. Updating a key
 * keeps its insertion-order position. */
NB_EXPORT bool nb_map_put(nb_map *map, nb_str key, void *value,
                          void **old_value);
NB_EXPORT bool nb_map_put_cstr(nb_map *map, const char *key, void *value,
                               void **old_value);

/* Returns a pointer to the value slot for key, inserting a new entry with a
 * NULL value when needed. *created is set to true only for a new entry.
 * Returns NULL on invalid arguments or allocation failure. The slot survives
 * rehash/insertion and is invalidated by deleting its key or clear/free. */
NB_EXPORT void **nb_map_slot(nb_map *map, nb_str key, bool *created);

/* Installs an optional bulk-cleanup destructor. See nb_map_value_dtor. */
NB_EXPORT void nb_map_set_value_dtor(nb_map *map, nb_map_value_dtor dtor,
                                     void *ctx);

/* Grows the bucket array to hold at least min_capacity entries without
 * rehashing, accounting for the internal 3/4 load factor. Existing entries
 * and their order are preserved. Returns false when the requested size
 * overflows or allocation fails. */
NB_EXPORT bool nb_map_reserve(nb_map *map, size_t min_capacity);
NB_EXPORT void *nb_map_get(const nb_map *map, nb_str key);
NB_EXPORT void *nb_map_get_cstr(const nb_map *map, const char *key);
NB_EXPORT bool nb_map_has(const nb_map *map, nb_str key);
NB_EXPORT bool nb_map_has_cstr(const nb_map *map, const char *key);

/* Returns the removed value, or NULL when the key was absent. */
NB_EXPORT void *nb_map_del(nb_map *map, nb_str key);
NB_EXPORT void *nb_map_del_cstr(nb_map *map, const char *key);
NB_EXPORT void nb_map_clear(nb_map *map);
NB_EXPORT size_t nb_map_size(const nb_map *map);
/* Number of hash buckets, not the number of entries the map can hold before
 * growing. */
NB_EXPORT size_t nb_map_capacity(const nb_map *map);

/* Iteration follows insertion order. Keys point into the map and stay valid
 * while the map is not modified. */
NB_EXPORT void nb_map_iter_init(const nb_map *map, nb_map_iter *iter);
NB_EXPORT bool nb_map_iter_next(nb_map_iter *iter, nb_str *key, void **value);

#ifdef __cplusplus
}
#endif

#endif /* NB_MAP_H */
