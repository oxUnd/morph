#include "test.h"

#include "nb/nbase.h"

typedef struct containers_alloc_state {
    bool fail_next;
    unsigned skip;
    size_t frees;
    size_t allocs;
    size_t reallocs;
} containers_alloc_state;

static void *containers_alloc(void *ctx, size_t size)
{
    containers_alloc_state *state = (containers_alloc_state *)ctx;
    if (state->skip != 0) {
        state->skip--;
        return malloc(size);
    }
    if (state->fail_next) {
        state->fail_next = false;
        return NULL;
    }
    void *ptr = malloc(size);
    if (ptr) {
        state->allocs++;
    }
    return ptr;
}

static void *containers_realloc(void *ctx, void *ptr, size_t old_size,
                                size_t new_size)
{
    containers_alloc_state *state = (containers_alloc_state *)ctx;
    (void)old_size;
    if (state->skip != 0) {
        state->skip--;
        return realloc(ptr, new_size);
    }
    if (state->fail_next) {
        state->fail_next = false;
        return NULL;
    }
    void *result = realloc(ptr, new_size);
    if (result) {
        state->reallocs++;
    }
    return result;
}

static void containers_free(void *ctx, void *ptr, size_t size)
{
    containers_alloc_state *state = (containers_alloc_state *)ctx;
    (void)size;
    state->frees++;
    free(ptr);
}

static void containers_oom_noop(size_t size, void *ctx)
{
    (void)size;
    (void)ctx;
}

static void containers_install_failing(containers_alloc_state *state,
                                       nb_allocator *alloc)
{
    alloc->alloc = containers_alloc;
    alloc->realloc = containers_realloc;
    alloc->free = containers_free;
    alloc->ctx = state;
    nb_mem_set_allocator(alloc);
}

static void test_buf_truncate(void)
{
    nb_buf *buf = nb_buf_new();
    NB_CHECK(buf != NULL);
    if (!buf) {
        return;
    }

    NB_CHECK(nb_buf_append_cstr(buf, "hello"));

    /* Equal and larger lengths are no-ops. */
    nb_buf_truncate(buf, 5u);
    NB_CHECK_INT(nb_buf_len(buf), 5u);
    NB_CHECK_STR(nb_buf_cstr(buf), "hello");
    nb_buf_truncate(buf, 100u);
    NB_CHECK_INT(nb_buf_len(buf), 5u);
    NB_CHECK_STR(nb_buf_cstr(buf), "hello");

    nb_buf_truncate(buf, 2u);
    NB_CHECK_INT(nb_buf_len(buf), 2u);
    NB_CHECK_STR(nb_buf_cstr(buf), "he");
    NB_CHECK_INT((unsigned char)buf->ptr[2], 0);

    nb_buf_truncate(buf, 0u);
    NB_CHECK_INT(nb_buf_len(buf), 0u);
    NB_CHECK_STR(nb_buf_cstr(buf), "");
    NB_CHECK_INT((unsigned char)buf->ptr[0], 0);

    /* A truncated buffer must remain appendable. */
    NB_CHECK(nb_buf_append_cstr(buf, "again"));
    nb_buf_truncate(buf, 1u);
    NB_CHECK_STR(nb_buf_cstr(buf), "a");
    nb_buf_free(buf);

    /* A buffer that never allocated storage is safe to truncate. */
    nb_buf *empty = nb_buf_new();
    NB_CHECK(empty != NULL);
    if (empty) {
        NB_CHECK(empty->ptr == NULL);
        nb_buf_truncate(empty, 0u);
        nb_buf_truncate(empty, 8u);
        NB_CHECK_INT(nb_buf_len(empty), 0u);
        NB_CHECK_STR(nb_buf_cstr(empty), "");
        nb_buf_free(empty);
    }

    nb_buf_truncate(NULL, 1u);
}

static void test_buf_shrink(void)
{
    nb_buf *buf = nb_buf_new_with_cap(4096u);
    NB_CHECK(buf != NULL);
    if (!buf) {
        return;
    }
    NB_CHECK(nb_buf_append_cstr(buf, "hello world"));
    nb_buf_consume(buf, 6u);
    size_t old_cap = buf->cap;
    NB_CHECK(nb_buf_shrink(buf));
    NB_CHECK(buf->cap <= old_cap);
    NB_CHECK_INT(buf->cap, 6u);
    NB_CHECK_INT(buf->off, 0u);
    NB_CHECK_STR(nb_buf_cstr(buf), "world");

    NB_CHECK(nb_buf_append_cstr(buf, "!"));
    NB_CHECK_STR(nb_buf_cstr(buf), "world!");

    nb_buf_clear(buf);
    NB_CHECK(nb_buf_shrink(buf));
    NB_CHECK_INT(buf->cap, 1u);
    NB_CHECK_STR(nb_buf_cstr(buf), "");
    nb_buf_free(buf);
}

static void check_i64_append(int64_t value, const char *digits)
{
    nb_buf *buf = nb_buf_new();
    NB_CHECK(buf != NULL);
    if (!buf) {
        return;
    }
    NB_CHECK(nb_buf_append_cstr(buf, "v="));
    NB_CHECK(nb_buf_append_i64(buf, value));
    NB_CHECK_STR(nb_buf_cstr(buf) + 2u, digits);
    NB_CHECK_INT(nb_buf_len(buf), 2u + strlen(digits));
    NB_CHECK_INT((unsigned char)buf->ptr[buf->len], 0);
    nb_buf_free(buf);
}

static void test_buf_append_integers(void)
{
    check_i64_append(0, "0");
    check_i64_append(42, "42");
    check_i64_append(-42, "-42");
    check_i64_append(-1, "-1");
    check_i64_append(INT64_MAX, "9223372036854775807");
    check_i64_append(INT64_MIN, "-9223372036854775808");

    nb_buf *buf = nb_buf_new();
    NB_CHECK(buf != NULL);
    if (!buf) {
        return;
    }
    NB_CHECK(nb_buf_append_u64(buf, 0u));
    NB_CHECK(nb_buf_append_char(buf, ','));
    NB_CHECK(nb_buf_append_u64(buf, UINT64_MAX));
    NB_CHECK(nb_buf_append_char(buf, ','));
    NB_CHECK(nb_buf_append_i64(buf, INT64_MIN));
    NB_CHECK_STR(nb_buf_cstr(buf),
                 "0,18446744073709551615,-9223372036854775808");
    NB_CHECK_INT((unsigned char)buf->ptr[buf->len], 0);

    NB_CHECK(!nb_buf_append_i64(NULL, 1));
    NB_CHECK(!nb_buf_append_u64(NULL, 1u));
    nb_buf_free(buf);
}

static void test_buf_failure_paths(void)
{
    nb_buf *buf = nb_buf_new();
    NB_CHECK(buf != NULL);
    if (!buf) {
        return;
    }

    /* Fill the initial 16-byte allocation so the next append must grow. */
    NB_CHECK(nb_buf_append(buf, "123456789012345", 15u));
    NB_CHECK(buf->cap == 16u);
    NB_CHECK_INT(buf->len, 15u);

    const nb_allocator *saved = nb_mem_allocator();
    containers_alloc_state state = { false };
    nb_allocator failing;
    containers_install_failing(&state, &failing);

    state.fail_next = true;
    NB_CHECK(!nb_buf_append_i64(buf, 42));
    NB_CHECK_INT(buf->len, 15u);
    NB_CHECK(buf->cap == 16u);
    NB_CHECK_MEM(buf->ptr, "123456789012345", 15u);
    NB_CHECK_INT((unsigned char)buf->ptr[15], 0);

    state.fail_next = true;
    NB_CHECK(!nb_buf_append_u64(buf, UINT64_MAX));
    NB_CHECK_INT(buf->len, 15u);
    NB_CHECK_MEM(buf->ptr, "123456789012345", 15u);

    state.fail_next = true;
    NB_CHECK(!nb_buf_append_char(buf, 'x'));
    NB_CHECK_INT(buf->len, 15u);
    NB_CHECK_MEM(buf->ptr, "123456789012345", 15u);

    NB_CHECK(!nb_buf_ok(buf));
    nb_buf_clear_error(buf);
    nb_mem_set_allocator(saved);

    NB_CHECK(nb_buf_append_char(buf, '!'));
    NB_CHECK_STR(nb_buf_cstr(buf), "123456789012345!");
    nb_buf_free(buf);

    /* A forged len == SIZE_MAX must be rejected instead of wrapping. */
    nb_buf forged = {NULL, SIZE_MAX, 0u, 0u, false, NB_OK};
    NB_CHECK(!nb_buf_reserve(&forged, 0u));
    NB_CHECK(!nb_buf_append(&forged, NULL, 0u));
    NB_CHECK(forged.ptr == NULL);
    NB_CHECK(forged.len == SIZE_MAX);
}

static void test_buf_reserve_bounds(void)
{
    nb_buf *buf = nb_buf_new();
    NB_CHECK(buf != NULL);
    if (!buf) {
        return;
    }

    NB_CHECK(nb_buf_reserve(buf, 0u));
    NB_CHECK(buf->ptr != NULL);
    NB_CHECK_INT((unsigned char)buf->ptr[0], 0);
    NB_CHECK(nb_buf_append_cstr(buf, "abc"));
    size_t cap = buf->cap;

    NB_CHECK(!nb_buf_reserve(buf, SIZE_MAX));
    NB_CHECK(!nb_buf_reserve(buf, SIZE_MAX - 3u));
    NB_CHECK(!nb_buf_append(buf, "x", SIZE_MAX));
    NB_CHECK_INT(buf->len, 3u);
    NB_CHECK(buf->cap == cap);
    NB_CHECK_STR(nb_buf_cstr(buf), "abc");

    NB_CHECK(!nb_buf_append(NULL, "x", 1u));
    NB_CHECK(!nb_buf_append(buf, NULL, 1u));
    NB_CHECK_INT(buf->len, 3u);
    nb_buf_free(buf);
}

static void test_buf_new_failure(void)
{
    const nb_allocator *saved = nb_mem_allocator();
    containers_alloc_state state = { false };
    nb_allocator failing;
    containers_install_failing(&state, &failing);
    nb_mem_set_oom_handler(containers_oom_noop, NULL);

    state.fail_next = true;
    nb_buf *allocated = nb_buf_new();
    NB_CHECK(allocated == NULL);
    nb_buf_free(allocated);

    state.fail_next = true;
    allocated = nb_buf_new_with_cap(8u);
    NB_CHECK(allocated == NULL);
    nb_buf_free(allocated);

    nb_mem_set_allocator(saved);
    nb_mem_set_oom_handler(NULL, NULL);
}

static void test_buf_consume(void)
{
    nb_buf *buf = nb_buf_new();
    NB_CHECK(buf != NULL);
    if (!buf) {
        return;
    }

    NB_CHECK(nb_buf_append_cstr(buf, "abcdef"));
    nb_buf_consume(buf, 2u);
    NB_CHECK_INT(nb_buf_len(buf), 4u);
    NB_CHECK_INT(buf->off, 2u);
    NB_CHECK_INT(buf->len, 6u);
    NB_CHECK_STR(nb_buf_cstr(buf), "cdef");

    /* Appends go to the absolute end; the unread prefix stays untouched. */
    NB_CHECK(nb_buf_append_cstr(buf, "gh"));
    NB_CHECK_STR(nb_buf_cstr(buf), "cdefgh");
    NB_CHECK_INT(nb_buf_len(buf), 6u);

    nb_buf_compact(buf);
    NB_CHECK_INT(buf->off, 0u);
    NB_CHECK_INT(buf->len, 6u);
    NB_CHECK_STR(nb_buf_cstr(buf), "cdefgh");

    nb_buf_consume(buf, 1u);
    nb_buf_truncate(buf, 3u);
    NB_CHECK_STR(nb_buf_cstr(buf), "de");
    NB_CHECK_INT(nb_buf_len(buf), 2u);

    nb_buf_truncate(buf, 0u);
    NB_CHECK_INT(nb_buf_len(buf), 0u);
    NB_CHECK_STR(nb_buf_cstr(buf), "");
    nb_buf_free(buf);

    nb_buf embedded;
    nb_buf_init(&embedded);
    NB_CHECK(nb_buf_append_cstr(&embedded, "embedded"));
    nb_buf moved;
    nb_buf_init(&moved);
    nb_buf_move(&moved, &embedded);
    NB_CHECK_STR(nb_buf_cstr(&moved), "embedded");
    NB_CHECK_STR(nb_buf_cstr(&embedded), "");
    nb_buf_destroy(&moved);
    nb_buf_destroy(&embedded);

    /* Consuming every visible byte must not invalidate an outstanding view. */
    nb_buf *whole = nb_buf_new();
    NB_CHECK(whole != NULL);
    if (whole) {
        NB_CHECK(nb_buf_append_cstr(whole, "view"));
        nb_str view = nb_buf_str(whole);
        nb_buf_consume(whole, nb_buf_len(whole));
        NB_CHECK_INT(nb_buf_len(whole), 0u);
        NB_CHECK_STR(nb_buf_cstr(whole), "");
        NB_CHECK_MEM(view.ptr, "view", 4u);
        NB_CHECK(nb_buf_append_cstr(whole, "next"));
        NB_CHECK_STR(nb_buf_cstr(whole), "next");
        nb_buf_compact(whole);
        NB_CHECK_STR(nb_buf_cstr(whole), "next");
        nb_buf_free(whole);
    }

    /* Reserve reclaims a consumed prefix once it exceeds half the capacity. */
    nb_buf *lazy = nb_buf_new();
    NB_CHECK(lazy != NULL);
    if (lazy) {
        char payload[100];
        memset(payload, 'a', sizeof(payload));
        NB_CHECK(nb_buf_append(lazy, payload, sizeof(payload)));
        nb_buf_consume(lazy, 90u);
        NB_CHECK(nb_buf_append_cstr(lazy, "XYZ"));
        NB_CHECK_INT(lazy->off, 0u);
        NB_CHECK_INT(nb_buf_len(lazy), 13u);
        NB_CHECK_STR(nb_buf_cstr(lazy), "aaaaaaaaaaXYZ");
        nb_buf_free(lazy);
    }
}

static void test_encoding_rollback_after_consume(void)
{
    nb_buf *out = nb_buf_new();
    NB_CHECK(out != NULL);
    if (!out) {
        return;
    }
    char payload[100];
    memset(payload, 'a', sizeof(payload));
    NB_CHECK(nb_buf_append(out, payload, sizeof(payload)));
    nb_buf_consume(out, 90u);
    NB_CHECK_STR(nb_buf_cstr(out), "aaaaaaaaaa");

    /* Encoding normalizes the consumed prefix before recording its rollback
     * mark, and a failed growth must restore exactly that visible content. */
    const nb_allocator *saved = nb_mem_allocator();
    containers_alloc_state state = { false, 0u, 0u, 0u, 0u };
    nb_allocator failing;
    containers_install_failing(&state, &failing);
    state.fail_next = true;
    nb_err err;
    nb_err_clear(&err);
    NB_CHECK_INT(nb_hex_encode(payload, sizeof(payload), true, out, &err),
                 NB_ERR_NOMEM);
    NB_CHECK_STR(nb_buf_cstr(out), "aaaaaaaaaa");
    NB_CHECK_INT(nb_buf_len(out), 10u);
    nb_mem_set_allocator(saved);
    nb_buf_free(out);
}

static void test_growth_policy(void)
{
    NB_CHECK(nb_arena_new(SIZE_MAX) == NULL);

    const nb_allocator *saved = nb_mem_allocator();
    containers_alloc_state state = { false };
    nb_allocator alloc;
    containers_install_failing(&state, &alloc);

    nb_arena *arena = nb_arena_new(1024u);
    NB_CHECK(arena != NULL);
    if (arena) {
        size_t before = state.allocs;
        for (size_t i = 0; i < 1024u; i++) {
            NB_CHECK(nb_arena_alloc(arena, 1024u) != NULL);
        }
        /* Geometric blocks keep the backing allocation count in the low
         * teens instead of one block per request. */
        NB_CHECK(state.allocs - before <= 16u);
        NB_CHECK(nb_arena_used(arena) >= 1024u * 1024u);

        size_t total = nb_arena_total(arena);
        size_t allocs = state.allocs;
        nb_arena_reset(arena);
        for (size_t i = 0; i < 1024u; i++) {
            NB_CHECK(nb_arena_alloc(arena, 1024u) != NULL);
        }
        NB_CHECK_INT(state.allocs, allocs);
        NB_CHECK_INT(nb_arena_total(arena), total);
        nb_arena_free(arena);
    }

    nb_pool *pool = nb_pool_new(16u, 8u);
    NB_CHECK(pool != NULL);
    if (pool) {
        size_t before = state.allocs;
        for (size_t i = 0; i < 100000u; i++) {
            NB_CHECK(nb_pool_alloc(pool) != NULL);
        }
        NB_CHECK(state.allocs - before <= 32u);
        NB_CHECK(nb_pool_capacity(pool) >= 100000u);
        NB_CHECK_INT(nb_pool_used(pool), 100000u);
        nb_pool_free(pool);
    }

    nb_buf *buf = nb_buf_new_with_cap(2000u);
    NB_CHECK(buf != NULL);
    if (buf) {
        char payload[2000];
        memset(payload, 'a', sizeof(payload));
        NB_CHECK(nb_buf_append(buf, payload, sizeof(payload)));
        nb_buf_consume(buf, 1900u);

        size_t allocs = state.allocs;
        size_t reallocs = state.reallocs;
        size_t cap = buf->cap;
        NB_CHECK(nb_buf_reserve(buf, 500u));
        /* Compacting the consumed prefix is enough here; growing would size
         * the buffer from bytes the caller already discarded. */
        NB_CHECK_INT(state.allocs, allocs);
        NB_CHECK_INT(state.reallocs, reallocs);
        NB_CHECK_INT(buf->cap, cap);
        NB_CHECK_INT(buf->off, 0u);
        NB_CHECK_INT(nb_buf_len(buf), 100u);
        NB_CHECK_MEM(nb_buf_cstr(buf), payload + 1900u, 100u);
        nb_buf_free(buf);
    }

    nb_mem_set_allocator(saved);
}

static void test_mem_allocator_snapshot(void)
{
    const nb_allocator *saved = nb_mem_allocator();
    containers_alloc_state first_state = { false, 0u, 0u, 0u, 0u };
    nb_allocator first;
    containers_install_failing(&first_state, &first);

    nb_arena *arena = nb_arena_new(1024u);
    NB_CHECK(arena != NULL);
    if (arena) {
        NB_CHECK(nb_arena_alloc(arena, 64u) != NULL);
    }
    nb_pool *pool = nb_pool_new(32u, 8u);
    NB_CHECK(pool != NULL);
    if (pool) {
        NB_CHECK(nb_pool_alloc(pool) != NULL);
    }
    size_t frees_before = first_state.frees;

    /* Existing arena/pool must keep the allocator they were created with. */
    containers_alloc_state second_state = { false, 0u, 0u, 0u, 0u };
    nb_allocator second;
    containers_install_failing(&second_state, &second);
    nb_arena_free(arena);
    nb_pool_free(pool);

    NB_CHECK(first_state.frees > frees_before);
    NB_CHECK_INT(second_state.frees, 0u);
    nb_mem_set_allocator(saved);
}

static void test_map_failure_atomicity(void)
{
    const nb_allocator *saved = nb_mem_allocator();
    containers_alloc_state state = { false, 0u, 0u, 0u, 0u };
    nb_allocator failing;
    containers_install_failing(&state, &failing);

    nb_map *map = nb_map_new();
    NB_CHECK(map != NULL);
    if (!map) {
        nb_mem_set_allocator(saved);
        return;
    }
    for (int i = 0; i < 6; i++) {
        char key[8];
        snprintf(key, sizeof(key), "k%d", i);
        NB_CHECK(nb_map_put_cstr(map, key, (void *)(intptr_t)(i + 1), NULL));
    }
    NB_CHECK_INT(nb_map_size(map), 6u);

    /* The entry comes from the existing slab; the following rehash
     * allocation is the one that fails. */
    state.fail_next = true;
    NB_CHECK(!nb_map_put_cstr(map, "k6", (void *)(intptr_t)7, NULL));
    NB_CHECK_INT(nb_map_size(map), 6u);
    NB_CHECK(nb_map_get_cstr(map, "k0") == (void *)(intptr_t)1);
    NB_CHECK(nb_map_get_cstr(map, "k6") == NULL);
    nb_map_free(map);
    nb_mem_set_allocator(saved);
}

static void test_vec_insert(void)
{
    nb_vec vec;
    nb_vec_init(&vec, sizeof(int));
    NB_CHECK(nb_vec_empty(&vec));
    NB_CHECK_INT(nb_vec_len(&vec), 0u);

    /* insert at 0 into an empty vector is an append. */
    int value = 10;
    NB_CHECK(nb_vec_insert(&vec, 0u, &value));
    NB_CHECK_INT(nb_vec_len(&vec), 1u);
    NB_CHECK_INT(*(int *)nb_vec_at(&vec, 0u), 10);

    value = 20;
    NB_CHECK(nb_vec_insert(&vec, 1u, &value)); /* index == len */
    value = 5;
    NB_CHECK(nb_vec_insert(&vec, 0u, &value));
    value = 15;
    NB_CHECK(nb_vec_insert(&vec, 2u, &value));
    value = 30;
    NB_CHECK(nb_vec_insert(&vec, 4u, &value));

    const int expected[] = { 5, 10, 15, 20, 30 };
    NB_CHECK_INT(nb_vec_len(&vec), NB_ARRAY_SIZE(expected));
    NB_CHECK_MEM(nb_vec_at(&vec, 0u), expected, sizeof(expected));

    value = 99;
    size_t len = nb_vec_len(&vec);
    NB_CHECK(!nb_vec_insert(&vec, len + 1u, &value));
    NB_CHECK(!nb_vec_insert(&vec, SIZE_MAX, &value));
    NB_CHECK(!nb_vec_insert(&vec, 0u, NULL));
    NB_CHECK(!nb_vec_insert(NULL, 0u, &value));
    NB_CHECK_INT(nb_vec_len(&vec), NB_ARRAY_SIZE(expected));
    NB_CHECK_MEM(nb_vec_at(&vec, 0u), expected, sizeof(expected));

    nb_vec_destroy(&vec);
}

static void test_vec_shrink(void)
{
    nb_vec vec;
    nb_vec_init(&vec, sizeof(int));
    for (int i = 0; i < 100; i++) {
        int *slot = nb_vec_push(&vec);
        NB_CHECK(slot != NULL);
        if (slot) {
            *slot = i;
        }
    }
    size_t old_cap = vec.cap;
    while (vec.len > 10u) {
        nb_vec_pop(&vec);
    }
    NB_CHECK(nb_vec_shrink(&vec));
    NB_CHECK(vec.cap <= old_cap);
    NB_CHECK_INT(vec.cap, 10u);
    for (int i = 0; i < 10; i++) {
        NB_CHECK_INT(*(int *)nb_vec_at(&vec, (size_t)i), i);
    }

    int value = 10;
    NB_CHECK(nb_vec_push_copy(&vec, &value));
    NB_CHECK_INT(*(int *)nb_vec_at(&vec, 10u), 10);

    nb_vec_clear(&vec);
    NB_CHECK(nb_vec_shrink(&vec));
    NB_CHECK(vec.data == NULL);
    NB_CHECK_INT(vec.cap, 0u);
    nb_vec_destroy(&vec);
}

static void test_vec_remove(void)
{
    nb_vec vec;
    nb_vec_init(&vec, sizeof(int));
    const int values[] = { 1, 2, 3, 4 };
    for (size_t i = 0; i < NB_ARRAY_SIZE(values); i++) {
        NB_CHECK(nb_vec_push_copy(&vec, &values[i]));
    }

    nb_vec_remove(&vec, 1u);
    const int after_middle[] = { 1, 3, 4 };
    NB_CHECK_INT(nb_vec_len(&vec), NB_ARRAY_SIZE(after_middle));
    NB_CHECK_MEM(nb_vec_at(&vec, 0u), after_middle, sizeof(after_middle));

    nb_vec_remove(&vec, 0u);
    const int after_first[] = { 3, 4 };
    NB_CHECK_INT(nb_vec_len(&vec), NB_ARRAY_SIZE(after_first));
    NB_CHECK_MEM(nb_vec_at(&vec, 0u), after_first, sizeof(after_first));

    nb_vec_remove(&vec, 1u);
    NB_CHECK_INT(nb_vec_len(&vec), 1u);
    NB_CHECK_INT(*(int *)nb_vec_at(&vec, 0u), 3);

    nb_vec_remove(&vec, 1u); /* index == len */
    nb_vec_remove(&vec, SIZE_MAX);
    NB_CHECK_INT(nb_vec_len(&vec), 1u);
    NB_CHECK_INT(*(int *)nb_vec_at(&vec, 0u), 3);

    nb_vec_remove(&vec, 0u);
    NB_CHECK(nb_vec_empty(&vec));
    nb_vec_remove(&vec, 0u); /* empty vector */
    NB_CHECK(nb_vec_empty(&vec));

    int value = 7;
    NB_CHECK(nb_vec_insert(&vec, 0u, &value));
    NB_CHECK_INT(*(int *)nb_vec_at(&vec, 0u), 7);

    nb_vec_remove(NULL, 0u);
    nb_vec_destroy(&vec);
}

static void test_vec_insert_failures(void)
{
    int values[8];
    nb_vec vec;
    nb_vec_init(&vec, sizeof(int));
    for (int i = 0; i < 8; i++) {
        values[i] = i * 10;
        NB_CHECK(nb_vec_push_copy(&vec, &values[i]));
    }
    NB_CHECK_INT(nb_vec_len(&vec), 8u);
    NB_CHECK(vec.cap == 8u);

    const nb_allocator *saved = nb_mem_allocator();
    containers_alloc_state state = { false };
    nb_allocator failing;
    containers_install_failing(&state, &failing);

    /* The vector is full, so insert must grow and fails without changing it. */
    int inserted = 99;
    state.fail_next = true;
    NB_CHECK(!nb_vec_insert(&vec, 3u, &inserted));
    NB_CHECK_INT(nb_vec_len(&vec), 8u);
    NB_CHECK_MEM(vec.data, values, sizeof(values));

    nb_vec empty;
    nb_vec_init(&empty, sizeof(int));
    state.fail_next = true;
    NB_CHECK(!nb_vec_insert(&empty, 0u, &inserted));
    NB_CHECK_INT(nb_vec_len(&empty), 0u);
    NB_CHECK(empty.data == NULL);
    nb_vec_destroy(&empty);

    nb_mem_set_allocator(saved);

    NB_CHECK(nb_vec_insert(&vec, 3u, &inserted));
    NB_CHECK_INT(nb_vec_len(&vec), 9u);
    NB_CHECK_INT(*(int *)nb_vec_at(&vec, 3u), 99);
    NB_CHECK_INT(*(int *)nb_vec_at(&vec, 4u), 30);
    NB_CHECK_INT(*(int *)nb_vec_at(&vec, 8u), 70);

    nb_vec_destroy(&vec);
}

static void test_vec_boundaries(void)
{
    int value = 1;

    /* A forged len == SIZE_MAX must not wrap len + 1. */
    nb_vec forged = { NULL, SIZE_MAX, 0u, sizeof(int) };
    NB_CHECK(!nb_vec_insert(&forged, 0u, &value));
    NB_CHECK(forged.len == SIZE_MAX);
    NB_CHECK(forged.data == NULL);

    nb_vec vec;
    nb_vec_init(&vec, sizeof(int));
    NB_CHECK(!nb_vec_reserve(&vec, SIZE_MAX));
    NB_CHECK(vec.data == NULL);
    NB_CHECK_INT(nb_vec_len(&vec), 0u);
    NB_CHECK(!nb_vec_insert(&vec, 1u, &value)); /* index > len */
    NB_CHECK_INT(nb_vec_len(&vec), 0u);
    nb_vec_destroy(&vec);
}

typedef struct queue_node {
    int value;
    nb_queue link;
} queue_node;

static int queue_node_cmp(const nb_queue *a, const nb_queue *b)
{
    const queue_node *left = NB_QUEUE_DATA(a, queue_node, link);
    const queue_node *right = NB_QUEUE_DATA(b, queue_node, link);
    return (left->value > right->value) - (left->value < right->value);
}

static void test_queue(void)
{
    nb_queue head;
    nb_queue tail;
    queue_node nodes[5] = {0};

    nb_queue_init(&head);
    NB_CHECK(nb_queue_empty(&head));
    NB_CHECK(nb_queue_head(&head) == &head);
    NB_CHECK(nb_queue_last(&head) == &head);
    NB_CHECK(nb_queue_middle(&head) == NULL);

    for (int i = 0; i < 5; i++) {
        nodes[i].value = i;
        nb_queue_insert_tail(&head, &nodes[i].link);
    }
    NB_CHECK(!nb_queue_empty(&head));
    NB_CHECK(NB_QUEUE_DATA(nb_queue_head(&head), queue_node, link) == &nodes[0]);
    NB_CHECK(NB_QUEUE_DATA(nb_queue_last(&head), queue_node, link) == &nodes[4]);

    nb_queue_remove(&nodes[1].link);
    NB_CHECK(nodes[1].link.next == NULL);
    NB_CHECK(nodes[1].link.prev == NULL);
    nb_queue_insert_head(&head, &nodes[1].link);

    nb_queue *link;
    int seen = 0;
    NB_QUEUE_FOREACH(link, &head)
    {
        NB_CHECK(NB_QUEUE_DATA(link, queue_node, link)->value >= 0);
        seen++;
    }
    NB_CHECK_INT(seen, 5);

    nb_queue_split(&head, &nodes[1].link, &tail);
    NB_CHECK(!nb_queue_empty(&tail));
    nb_queue_add(&head, &tail);
    NB_CHECK(nb_queue_empty(&tail));
    NB_CHECK(nb_queue_middle(&head) != NULL);

    nb_queue_sort(&head, queue_node_cmp);
    int previous = -1;
    seen = 0;
    NB_QUEUE_FOREACH(link, &head)
    {
        int value = NB_QUEUE_DATA(link, queue_node, link)->value;
        NB_CHECK(value >= previous);
        previous = value;
        seen++;
    }
    NB_CHECK_INT(seen, 5);
}

static void test_vec_batch_and_move(void)
{
    nb_vec first;
    nb_vec second;
    nb_vec_init(&first, sizeof(int));
    nb_vec_init(&second, sizeof(int));
    int *slots = nb_vec_push_n(&first, 3);
    NB_CHECK(slots != NULL);
    slots[0] = 11;
    slots[1] = 22;
    slots[2] = 33;
    NB_CHECK(nb_vec_push_n(&first, SIZE_MAX) == NULL);
    NB_CHECK_INT(first.len, 3);
    NB_CHECK(nb_vec_push_copy(&second, &slots[1]));
    nb_vec_move(&first, &second);
    NB_CHECK_INT(first.len, 1);
    NB_CHECK_INT(*(int *)nb_vec_at(&first, 0), 22);
    NB_CHECK_INT(second.len, 3);
    NB_CHECK_INT(*(int *)nb_vec_at(&second, 2), 33);
    nb_vec_remove_range(&second, 1, SIZE_MAX);
    NB_CHECK_INT(second.len, 1);
    NB_CHECK_INT(*(int *)nb_vec_at(&second, 0), 11);
    nb_vec_destroy(&first);
    nb_vec_destroy(&second);
}

static void test_buf_status(void)
{
    nb_buf value;
    nb_buf_init(&value);
    NB_CHECK_INT(nb_buf_status(&value), NB_OK);
    nb_buf_set_error(&value, NB_ERR_INVAL);
    NB_CHECK(!nb_buf_append_cstr(&value, "rejected"));
    nb_buf_set_error(&value, NB_ERR_NOMEM);
    NB_CHECK_INT(nb_buf_status(&value), NB_ERR_INVAL);
    nb_buf_clear_error(&value);
    value.error = true;
    NB_CHECK_INT(nb_buf_status(&value), NB_ERR_NOMEM);
    nb_buf_clear(&value);
    NB_CHECK_INT(nb_buf_status(&value), NB_OK);
    NB_CHECK(nb_buf_append_cstr(&value, "accepted"));
    nb_buf_destroy(&value);
}

void test_containers(void)
{
    test_buf_status();
    test_buf_truncate();
    test_buf_shrink();
    test_buf_append_integers();
    test_buf_failure_paths();
    test_buf_reserve_bounds();
    test_buf_new_failure();
    test_buf_consume();
    test_encoding_rollback_after_consume();
    test_growth_policy();
    test_mem_allocator_snapshot();
    test_map_failure_atomicity();
    test_vec_insert();
    test_vec_shrink();
    test_vec_remove();
    test_vec_insert_failures();
    test_vec_boundaries();
    test_queue();
    test_vec_batch_and_move();
}
