#include "nb/nbase.h"

#include <inttypes.h>

/* Count requested backing storage, excluding allocator metadata/RSS. */
typedef struct counters {
    size_t live;
    size_t allocations;
} counters;

static void require(bool condition)
{
    if (!condition) {
        fputs("benchmark operation failed\n", stderr);
        exit(1);
    }
}

static void *count_alloc(void *ctx, size_t size)
{
    counters *state = ctx;
    void *ptr = malloc(size);
    if (ptr) {
        state->live += size;
        state->allocations++;
    }
    return ptr;
}

static void *count_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size)
{
    counters *state = ctx;
    void *result = realloc(ptr, new_size);
    if (result) {
        state->live = state->live - old_size + new_size;
        state->allocations++;
    }
    return result;
}

static void count_free(void *ctx, void *ptr, size_t size)
{
    counters *state = ctx;
    state->live -= size;
    free(ptr);
}

static void report(const char *name, int64_t start, const counters *state)
{
    printf("%s,%.3f,%zu,%zu\n", name,
           (double)(nb_time_monotonic_ns() - start) / 1e6,
           state->live, state->allocations);
}

static void bench_stream(counters *state)
{
    nb_buf buf;
    nb_buf_init(&buf);
    char data[65535];
    memset(data, 'x', sizeof(data));
    require(nb_buf_append(&buf, data, sizeof(data)));
    int64_t start = nb_time_monotonic_ns();
    for (size_t i = 0; i < 200000u; i++) {
        nb_buf_consume(&buf, 1u);
        require(nb_buf_append_char(&buf, 'x'));
    }
    require(nb_buf_len(&buf) == sizeof(data));
    report("buf_stream_200k", start, state);
    nb_buf_destroy(&buf);
}

static void bench_arena(counters *state)
{
    nb_arena *arena = nb_arena_new(1024u);
    require(arena != NULL);
    int64_t start = nb_time_monotonic_ns();
    for (size_t phase = 0; phase < 20u; phase++) {
        nb_arena_reset(arena);
        for (size_t i = 0; i < 100u; i++) {
            void *data = nb_arena_alloc(arena, 1024u);
            require(data != NULL);
            memset(data, 0x5a, 1024u);
        }
    }
    report("arena_reset_20x100", start, state);
    nb_arena_free(arena);
}

static void bench_encoding(counters *state)
{
    nb_buf out;
    nb_buf_init(&out);
    unsigned char data[65536];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (unsigned char)i;
    }
    int64_t start = nb_time_monotonic_ns();
    for (size_t i = 0; i < 500u; i++) {
        nb_buf_clear(&out);
        require(nb_base64_encode(data, sizeof(data), NB_B64_STD, true, &out, NULL) == NB_OK);
    }
    require(nb_buf_len(&out) == 87384u);
    report("base64_500x64k", start, state);
    nb_buf_destroy(&out);
    start = nb_time_monotonic_ns();
    for (size_t i = 0; i < 500u; i++) {
        nb_buf_clear(&out);
        require(nb_hex_encode(data, sizeof(data), false, &out, NULL) == NB_OK);
    }
    require(nb_buf_len(&out) == 131072u);
    report("hex_500x64k", start, state);
    nb_buf_destroy(&out);
}

static void bench_map(counters *state, size_t key_len)
{
    nb_map *map = nb_map_new();
    require(map != NULL && nb_map_reserve(map, 10000u));
    char keys[10000][128];
    require(key_len < sizeof(keys[0]));
    for (size_t i = 0; i < 10000u; i++) {
        memset(keys[i], 'x', key_len);
        snprintf(keys[i], 9u, "%08zu", i);
        if (key_len > 8u) {
            keys[i][8] = 'x';
        }
    }
    int64_t start = nb_time_monotonic_ns();
    for (size_t i = 0; i < 10000u; i++) {
        require(nb_map_put(map, nb_str_from_parts(keys[i], key_len),
                           (void *)(uintptr_t)(i + 1u), NULL));
    }
    for (size_t pass = 0; pass < 20u; pass++) {
        for (size_t i = 0; i < 10000u; i++) {
            require(nb_map_get(map, nb_str_from_parts(keys[i], key_len)) ==
                    (void *)(uintptr_t)(i + 1u));
        }
    }
    char name[64];
    snprintf(name, sizeof(name), "map_10k_key%zu", key_len);
    report(name, start, state);
    start = nb_time_monotonic_ns();
    for (size_t pass = 0; pass < 10u; pass++) {
        for (size_t i = 0; i < 10000u; i++) {
            nb_str key = nb_str_from_parts(keys[i], key_len);
            require(nb_map_del(map, key) == (void *)(uintptr_t)(i + 1u));
            require(nb_map_put(map, key, (void *)(uintptr_t)(i + 1u), NULL));
        }
    }
    snprintf(name, sizeof(name), "map_churn_100k_key%zu", key_len);
    report(name, start, state);
    nb_map_free(map);
}

int main(void)
{
    counters state = { 0 };
    nb_allocator alloc = { count_alloc, count_realloc, count_free, &state };
    const nb_allocator *saved = nb_mem_allocator();
    nb_mem_set_allocator(&alloc);
    puts("scenario,milliseconds,live_bytes,allocation_calls");
    bench_stream(&state);
    require(state.live == 0);
    state.allocations = 0;
    bench_arena(&state);
    require(state.live == 0);
    state.allocations = 0;
    bench_encoding(&state);
    require(state.live == 0);
    state.allocations = 0;
    bench_map(&state, 8u);
    require(state.live == 0);
    state.allocations = 0;
    bench_map(&state, 64u);
    require(state.live == 0);
    nb_mem_set_allocator(saved);
    return 0;
}
