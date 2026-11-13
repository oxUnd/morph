#include "nb/mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NB_ALIGN 16u
#define NB_ALIGN_UP(n) (((n) + (NB_ALIGN - 1u)) & ~((size_t)NB_ALIGN - 1u))
#define NB_ARENA_DEFAULT_BLOCK (64u * 1024u)
#define NB_ARENA_MIN_BLOCK 1024u
#define NB_ARENA_MAX_BLOCK (4u * 1024u * 1024u)
#define NB_POOL_DEFAULT_COUNT 64u
#define NB_POOL_MIN_COUNT 8u
#define NB_POOL_MAX_SLAB_BYTES (1024u * 1024u)

static void *default_alloc(void *ctx, size_t size)
{
    (void)ctx;
    return malloc(size ? size : 1u);
}

static void *default_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size)
{
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size ? new_size : 1u);
}

static void default_free(void *ctx, void *ptr, size_t size)
{
    (void)ctx;
    (void)size;
    free(ptr);
}

static nb_allocator default_allocator = {
    default_alloc,
    default_realloc,
    default_free,
    NULL
};

static void default_oom_handler(size_t size, void *ctx)
{
    (void)ctx;
    fprintf(stderr, "nbase: out of memory allocating %zu bytes\n", size);
    fflush(stderr);
    abort();
}

static _Atomic(const nb_allocator *) g_allocator = &default_allocator;
static _Atomic(nb_oom_handler) g_oom_handler = default_oom_handler;
static _Atomic(void *) g_oom_ctx = NULL;

const nb_allocator *nb_mem_allocator(void)
{
    return atomic_load_explicit(&g_allocator, memory_order_acquire);
}

void nb_mem_set_allocator(const nb_allocator *alloc)
{
    if (!alloc) {
        alloc = &default_allocator;
    }
    NB_ASSERT(alloc->alloc != NULL);
    NB_ASSERT(alloc->realloc != NULL);
    NB_ASSERT(alloc->free != NULL);
    atomic_store_explicit(&g_allocator, alloc, memory_order_release);
}

void nb_mem_set_oom_handler(nb_oom_handler handler, void *ctx)
{
    if (!handler) {
        handler = default_oom_handler;
        ctx = NULL;
    }
    atomic_store_explicit(&g_oom_ctx, ctx, memory_order_release);
    atomic_store_explicit(&g_oom_handler, handler, memory_order_release);
}

void *nb_try_alloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    const nb_allocator *alloc = nb_mem_allocator();
    return alloc->alloc(alloc->ctx, size);
}

void *nb_alloc(size_t size)
{
    void *ptr = nb_try_alloc(size);
    if (NB_UNLIKELY(!ptr && size != 0)) {
        nb_oom_handler handler = atomic_load_explicit(&g_oom_handler, memory_order_acquire);
        void *ctx = atomic_load_explicit(&g_oom_ctx, memory_order_acquire);
        handler(size, ctx);
        /* A custom handler must not return; recovery paths use nb_try_alloc. */
        abort();
    }
    return ptr;
}

void *nb_alloc0(size_t size)
{
    void *ptr = nb_alloc(size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void *nb_try_realloc(void *ptr, size_t old_size, size_t new_size)
{
    if (!ptr) {
        return nb_try_alloc(new_size);
    }
    const nb_allocator *alloc = nb_mem_allocator();
    if (new_size == 0) {
        alloc->free(alloc->ctx, ptr, old_size);
        return NULL;
    }
    return alloc->realloc(alloc->ctx, ptr, old_size, new_size);
}

void *nb_realloc(void *ptr, size_t old_size, size_t new_size)
{
    void *result = nb_try_realloc(ptr, old_size, new_size);
    if (NB_UNLIKELY(!result && new_size != 0)) {
        nb_oom_handler handler = atomic_load_explicit(&g_oom_handler, memory_order_acquire);
        void *ctx = atomic_load_explicit(&g_oom_ctx, memory_order_acquire);
        handler(new_size, ctx);
        abort();
    }
    return result;
}

void nb_free(void *ptr, size_t size)
{
    if (!ptr) {
        return;
    }
    const nb_allocator *alloc = nb_mem_allocator();
    alloc->free(alloc->ctx, ptr, size);
}

/* ------------------------------------------------------------------------- */
/* Arena                                                                     */

typedef struct nb_arena_block {
    struct nb_arena_block *next;
    size_t cap;
    size_t used;
} nb_arena_block;

#define NB_ARENA_HDR NB_ALIGN_UP(sizeof(nb_arena_block))

struct nb_arena {
    const nb_allocator *alloc;
    nb_arena_block *blocks;
    nb_arena_block *head;
    nb_arena_block *tail;
    size_t block_size;
    size_t next_block_size;
    size_t total;
    size_t used;
    size_t peak;
};

static unsigned char *arena_block_data(nb_arena_block *block)
{
    return (unsigned char *)block + NB_ARENA_HDR;
}

nb_arena *nb_arena_new(size_t block_size)
{
    size_t requested =
        block_size ? NB_ALIGN_UP(block_size) : NB_ARENA_DEFAULT_BLOCK;
    if (block_size != 0 && requested < block_size) {
        return NULL;
    }
    const nb_allocator *alloc = nb_mem_allocator();
    nb_arena *arena = alloc->alloc(alloc->ctx, sizeof(*arena));
    if (!arena) {
        return NULL;
    }
    arena->alloc = alloc;
    arena->blocks = NULL;
    arena->head = NULL;
    arena->tail = NULL;
    arena->block_size = requested;
    if (arena->block_size < NB_ARENA_MIN_BLOCK) {
        arena->block_size = NB_ARENA_MIN_BLOCK;
    }
    arena->next_block_size = arena->block_size;
    arena->total = 0;
    arena->used = 0;
    arena->peak = 0;
    return arena;
}

void nb_arena_free(nb_arena *arena)
{
    if (!arena) {
        return;
    }
    const nb_allocator *alloc = arena->alloc;
    nb_arena_block *block = arena->blocks;
    while (block) {
        nb_arena_block *next = block->next;
        alloc->free(alloc->ctx, block, NB_ARENA_HDR + block->cap);
        block = next;
    }
    alloc->free(alloc->ctx, arena, sizeof(*arena));
}

void nb_arena_reset(nb_arena *arena)
{
    if (!arena) {
        return;
    }
    for (nb_arena_block *block = arena->blocks; block; block = block->next) {
        block->used = 0;
    }
    arena->head = arena->blocks;
    /* Retained blocks are reused first; new growth starts at the configured
     * base size again so a short phase does not permanently inflate blocks. */
    arena->next_block_size = arena->block_size;
    arena->used = 0;
}

void *nb_arena_alloc(nb_arena *arena, size_t size)
{
    if (!arena || size == 0) {
        return NULL;
    }
    size_t need = NB_ALIGN_UP(size);
    if (need < size) {
        return NULL;
    }
    nb_arena_block *block = arena->head;
    /* Blocks are kept in allocation order. Moving forward through retained
     * blocks preserves checkpoint ordering after reset as well as reuse. */
    while (block && block->cap - block->used < need) {
        block = block->next;
    }
    if (block && block->cap - block->used >= need) {
        arena->head = block;
        void *ptr = arena_block_data(block) + block->used;
        block->used += need;
        arena->used += need;
        if (arena->used > arena->peak) {
            arena->peak = arena->used;
        }
        return ptr;
    }

    /* Grow block capacities geometrically. This bounds the number of backing
     * allocations for long-lived arenas and keeps tail room usable for the
     * next few requests instead of leaving a nearly-empty block behind. */
    size_t cap = arena->next_block_size;
    while (cap < need && cap <= NB_ARENA_MAX_BLOCK / 2u) {
        cap *= 2u;
    }
    if (cap < need) {
        cap = need;
    }
    if (cap > SIZE_MAX - NB_ARENA_HDR ||
        arena->total > SIZE_MAX - cap) {
        return NULL;
    }
    block =
        arena->alloc->alloc(arena->alloc->ctx, NB_ARENA_HDR + cap);
    if (!block) {
        return NULL;
    }
    block->next = NULL;
    block->cap = cap;
    block->used = need;
    if (arena->tail) {
        arena->tail->next = block;
    } else {
        arena->blocks = block;
    }
    arena->tail = block;
    arena->head = block;
    arena->total += cap;
    arena->used += need;
    if (cap <= NB_ARENA_MAX_BLOCK) {
        size_t next = cap;
        if (next < NB_ARENA_MAX_BLOCK) {
            next = next > NB_ARENA_MAX_BLOCK / 2u
                       ? NB_ARENA_MAX_BLOCK
                       : next * 2u;
        }
        arena->next_block_size = next;
    }
    if (arena->used > arena->peak) {
        arena->peak = arena->used;
    }
    return arena_block_data(block);
}

void *nb_arena_checkpoint(const nb_arena *arena)
{
    if (!arena || !arena->head) {
        return NULL;
    }
    return arena_block_data(arena->head) + arena->head->used;
}

void nb_arena_release(nb_arena *arena, void *mark)
{
    if (!arena) {
        return;
    }
    const nb_allocator *alloc = arena->alloc;
    if (!mark) {
        nb_arena_block *block = arena->blocks;
        while (block) {
            nb_arena_block *next = block->next;
            alloc->free(alloc->ctx, block, NB_ARENA_HDR + block->cap);
            block = next;
        }
        arena->blocks = NULL;
        arena->head = NULL;
        arena->tail = NULL;
        arena->total = 0;
        arena->used = 0;
        arena->next_block_size = arena->block_size;
        return;
    }

    uintptr_t target_address = (uintptr_t)mark;
    nb_arena_block *target = NULL;
    for (nb_arena_block *block = arena->blocks; block; block = block->next) {
        uintptr_t start = (uintptr_t)arena_block_data(block);
        if (target_address >= start &&
            target_address - start <= block->used) {
            target = block;
            break;
        }
    }
    NB_ASSERT(target != NULL);
    if (!target) {
        return;
    }

    nb_arena_block *block = target->next;
    while (block) {
        nb_arena_block *next = block->next;
        size_t cap = block->cap;
        alloc->free(alloc->ctx, block, NB_ARENA_HDR + cap);
        arena->total -= cap;
        block = next;
    }
    target->next = NULL;
    arena->tail = target;
    target->used = target_address - (uintptr_t)arena_block_data(target);
    arena->head = target;
    arena->used = 0;
    for (block = arena->blocks; block; block = block->next) {
        arena->used += block->used;
    }
    arena->next_block_size = arena->block_size;
}

void *nb_arena_alloc0(nb_arena *arena, size_t size)
{
    void *ptr = nb_arena_alloc(arena, size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

nb_str nb_arena_strdup(nb_arena *arena, nb_str s)
{
    if (!arena || (s.len != 0 && !s.ptr) || s.len == SIZE_MAX) {
        return NB_STR_NULL;
    }
    char *copy = nb_arena_alloc(arena, s.len + 1u);
    if (!copy) {
        return NB_STR_NULL;
    }
    if (s.len != 0) {
        memcpy(copy, s.ptr, s.len);
    }
    copy[s.len] = '\0';
    return nb_str_from_parts(copy, s.len);
}

void *nb_arena_memdup(nb_arena *arena, const void *data, size_t len)
{
    if (!arena || len == 0 || !data) {
        return NULL;
    }
    void *copy = nb_arena_alloc(arena, len);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, data, len);
    return copy;
}

size_t nb_arena_total(const nb_arena *arena)
{
    return arena ? arena->total : 0;
}

size_t nb_arena_used(const nb_arena *arena)
{
    return arena ? arena->used : 0;
}

size_t nb_arena_peak(const nb_arena *arena)
{
    return arena ? arena->peak : 0;
}

size_t nb_arena_cached(const nb_arena *arena)
{
    return arena && arena->total >= arena->used
               ? arena->total - arena->used
               : 0;
}

/* ------------------------------------------------------------------------- */
/* Fixed-size pool                                                           */

typedef struct nb_pool_slab {
    struct nb_pool_slab *next;
    size_t count;
} nb_pool_slab;

#define NB_POOL_HDR NB_ALIGN_UP(sizeof(nb_pool_slab))

struct nb_pool {
    const nb_allocator *alloc;
    size_t elem_size;
    size_t slab_count;
    size_t next_count;
    size_t capacity;
    size_t used;
    size_t peak;
    nb_pool_slab *slabs;
    void *free_list;
};

static unsigned char *pool_slab_data(nb_pool_slab *slab)
{
    return (unsigned char *)slab + NB_POOL_HDR;
}

static void pool_push_free(nb_pool *pool, void *ptr)
{
    *(void **)ptr = pool->free_list;
    pool->free_list = ptr;
}

#ifndef NDEBUG
static bool pool_owns(const nb_pool *pool, const void *ptr)
{
    uintptr_t p = (uintptr_t)ptr;
    for (const nb_pool_slab *slab = pool->slabs; slab; slab = slab->next) {
        uintptr_t data = (uintptr_t)pool_slab_data((nb_pool_slab *)slab);
        size_t bytes = slab->count * pool->elem_size;
        if (p >= data && p - data < bytes &&
            (p - data) % pool->elem_size == 0) {
            return true;
        }
    }
    return false;
}
#endif

nb_pool *nb_pool_new(size_t elem_size, size_t initial_count)
{
    if (elem_size == 0) {
        return NULL;
    }
    size_t aligned = NB_ALIGN_UP(elem_size);
    if (aligned < sizeof(void *)) {
        aligned = NB_ALIGN_UP(sizeof(void *));
    }
    if (aligned < elem_size) {
        return NULL;
    }
    const nb_allocator *alloc = nb_mem_allocator();
    nb_pool *pool = alloc->alloc(alloc->ctx, sizeof(*pool));
    if (!pool) {
        return NULL;
    }
    pool->alloc = alloc;
    pool->elem_size = aligned;
    pool->slab_count =
        initial_count ? initial_count : NB_POOL_DEFAULT_COUNT;
    if (pool->slab_count < NB_POOL_MIN_COUNT) {
        pool->slab_count = NB_POOL_MIN_COUNT;
    }
    pool->next_count = pool->slab_count;
    pool->capacity = 0;
    pool->used = 0;
    pool->peak = 0;
    pool->slabs = NULL;
    pool->free_list = NULL;
    return pool;
}

void nb_pool_free(nb_pool *pool)
{
    if (!pool) {
        return;
    }
    const nb_allocator *alloc = pool->alloc;
    nb_pool_slab *slab = pool->slabs;
    while (slab) {
        nb_pool_slab *next = slab->next;
        alloc->free(alloc->ctx, slab,
                    NB_POOL_HDR + slab->count * pool->elem_size);
        slab = next;
    }
    alloc->free(alloc->ctx, pool, sizeof(*pool));
}

static bool pool_grow(nb_pool *pool)
{
    /* Double the slab element count until a slab reaches the byte cap. Large
     * elements fall back to one element per slab rather than allocating a
     * multi-megabyte slab for a small pool. */
    size_t count = pool->next_count;
    size_t max_count = NB_POOL_MAX_SLAB_BYTES / pool->elem_size;
    if (max_count < 1u) {
        max_count = 1u;
    }
    if (count > max_count) {
        count = max_count;
    }
    if (count > (SIZE_MAX - NB_POOL_HDR) / pool->elem_size) {
        return false;
    }
    if (pool->capacity > SIZE_MAX - count) {
        return false;
    }
    size_t bytes = NB_POOL_HDR + count * pool->elem_size;
    nb_pool_slab *slab = pool->alloc->alloc(pool->alloc->ctx, bytes);
    if (!slab) {
        return false;
    }
    slab->next = pool->slabs;
    slab->count = count;
    pool->slabs = slab;
    pool->capacity += count;
    pool->next_count =
        count >= max_count ? max_count : count * 2u;

    unsigned char *data = pool_slab_data(slab);
    for (size_t i = 0; i < count; i++) {
        pool_push_free(pool, data + i * pool->elem_size);
    }
    return true;
}

void *nb_pool_alloc(nb_pool *pool)
{
    if (!pool) {
        return NULL;
    }
    if (!pool->free_list && !pool_grow(pool)) {
        return NULL;
    }
    void *ptr = pool->free_list;
    pool->free_list = *(void **)ptr;
    pool->used++;
    if (pool->used > pool->peak) {
        pool->peak = pool->used;
    }
    return ptr;
}

void *nb_pool_alloc0(nb_pool *pool)
{
    void *ptr = nb_pool_alloc(pool);
    if (ptr) {
        memset(ptr, 0, pool->elem_size);
    }
    return ptr;
}

void nb_pool_release(nb_pool *pool, void *ptr)
{
    if (!pool || !ptr) {
        return;
    }
#ifndef NDEBUG
    NB_ASSERT(pool_owns(pool, ptr));
#endif
    NB_ASSERT(pool->used > 0);
    pool_push_free(pool, ptr);
    pool->used--;
}

void nb_pool_reset(nb_pool *pool)
{
    if (!pool) {
        return;
    }
    pool->free_list = NULL;
    for (nb_pool_slab *slab = pool->slabs; slab; slab = slab->next) {
        unsigned char *data = pool_slab_data(slab);
        for (size_t i = 0; i < slab->count; i++) {
            pool_push_free(pool, data + i * pool->elem_size);
        }
    }
    pool->used = 0;
}

size_t nb_pool_capacity(const nb_pool *pool)
{
    return pool ? pool->capacity : 0;
}

size_t nb_pool_used(const nb_pool *pool)
{
    return pool ? pool->used : 0;
}

size_t nb_pool_peak(const nb_pool *pool)
{
    return pool ? pool->peak : 0;
}

size_t nb_pool_cached(const nb_pool *pool)
{
    return pool && pool->capacity >= pool->used
               ? pool->capacity - pool->used
               : 0;
}
