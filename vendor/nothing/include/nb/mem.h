#ifndef NB_MEM_H
#define NB_MEM_H

#include "nb/base.h"
#include "nb/str.h"

#ifndef __cplusplus
#include <stdatomic.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Allocator interface. Sizes are passed explicitly so that an allocator can
 * keep accounting without hidden headers. A custom allocator must be fully
 * installed before any nbase object is created. Arenas and pools capture the
 * allocator at creation and use that snapshot for every block and slab; the
 * allocator object must outlive those objects.
 */
typedef struct nb_allocator {
    void *(*alloc)(void *ctx, size_t size);
    void *(*realloc)(void *ctx, void *ptr, size_t old_size, size_t new_size);
    void (*free)(void *ctx, void *ptr, size_t size);
    void *ctx;
} nb_allocator;

/* Invoked when an aborting allocation fails. The handler must not return:
 * nb_alloc/nb_realloc abort after it does. Recovery paths use the nb_try_*
 * functions instead. */
typedef void (*nb_oom_handler)(size_t size, void *ctx);

NB_EXPORT const nb_allocator *nb_mem_allocator(void);
NB_EXPORT void nb_mem_set_allocator(const nb_allocator *alloc);
NB_EXPORT void nb_mem_set_oom_handler(nb_oom_handler handler, void *ctx);

/* Aborting allocation helpers. A zero size returns NULL; a non-zero
 * allocation either succeeds or terminates the process. */
NB_EXPORT void *nb_alloc(size_t size);
NB_EXPORT void *nb_alloc0(size_t size);
NB_EXPORT void *nb_realloc(void *ptr, size_t old_size, size_t new_size);
NB_EXPORT void nb_free(void *ptr, size_t size);

/* Fallible variants. They return NULL on failure instead of invoking the OOM
 * handler. nb_try_realloc(ptr, old, 0) frees ptr and returns NULL. */
NB_EXPORT void *nb_try_alloc(size_t size);
NB_EXPORT void *nb_try_realloc(void *ptr, size_t old_size, size_t new_size);

typedef struct nb_arena nb_arena;

/* block_size is the initial data capacity of a new arena block. 0 uses
 * 64 KiB. Blocks grow geometrically (up to an internal cap) so long-lived
 * arenas do not create one OS allocation per small bump. nb_arena_reset()
 * reuses retained blocks before any new block is requested. */
NB_EXPORT nb_arena *nb_arena_new(size_t block_size);
NB_EXPORT void nb_arena_free(nb_arena *arena);
NB_EXPORT void nb_arena_reset(nb_arena *arena);
/* Records the current bump position. Allocations made after a checkpoint can
 * be discarded with nb_arena_release(); the mark is invalid after reset/free.
 * A NULL checkpoint means the arena is empty and releases all retained
 * blocks. */
NB_EXPORT void *nb_arena_checkpoint(const nb_arena *arena);
NB_EXPORT void nb_arena_release(nb_arena *arena, void *mark);

/* Arena allocations are fallible and return NULL when a block cannot be
 * obtained. Pointers stay valid until reset/free, or until release discards
 * their allocation. */
NB_EXPORT void *nb_arena_alloc(nb_arena *arena, size_t size);
NB_EXPORT void *nb_arena_alloc0(nb_arena *arena, size_t size);
/* Copies s into the arena and returns a borrowed view of the copy. Returns
 * NB_STR_NULL on allocation failure. */
NB_EXPORT nb_str nb_arena_strdup(nb_arena *arena, nb_str s);
/* Copies len bytes into the arena. Returns NULL for len == 0 or failure. */
NB_EXPORT void *nb_arena_memdup(nb_arena *arena, const void *data, size_t len);
NB_EXPORT size_t nb_arena_total(const nb_arena *arena);
NB_EXPORT size_t nb_arena_used(const nb_arena *arena);
NB_EXPORT size_t nb_arena_peak(const nb_arena *arena);
NB_EXPORT size_t nb_arena_cached(const nb_arena *arena);

typedef struct nb_pool nb_pool;

/* A pool of fixed-size elements. elem_size is rounded up for alignment.
 * initial_count defaults to 64 when 0 and is capped so one slab holds at
 * most 1 MiB of elements (never fewer than one). Slabs grow geometrically
 * after that and are retained for reuse by nb_pool_reset(). */
NB_EXPORT nb_pool *nb_pool_new(size_t elem_size, size_t initial_count);
NB_EXPORT void nb_pool_free(nb_pool *pool);

/* Returns NULL only when the backing allocator fails. The pool grows by
 * allocating another slab. */
NB_EXPORT void *nb_pool_alloc(nb_pool *pool);
NB_EXPORT void *nb_pool_alloc0(nb_pool *pool);
/* Returns the element to the free list. Debug builds verify that ptr belongs
 * to the pool; release builds trust the caller and do not scan slabs. */
NB_EXPORT void nb_pool_release(nb_pool *pool, void *ptr);
NB_EXPORT void nb_pool_reset(nb_pool *pool);
NB_EXPORT size_t nb_pool_capacity(const nb_pool *pool);
NB_EXPORT size_t nb_pool_used(const nb_pool *pool);
NB_EXPORT size_t nb_pool_peak(const nb_pool *pool);
NB_EXPORT size_t nb_pool_cached(const nb_pool *pool);

#ifdef __cplusplus
}
#endif

#endif /* NB_MEM_H */
