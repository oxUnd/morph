#include "test.h"

#include "nb/base.h"
#include "nb/buf.h"
#include "nb/err.h"
#include "nb/log.h"
#include "nb/map.h"
#include "nb/mem.h"
#include "nb/str.h"
#include "nb/time.h"
#include "nb/vec.h"

#include <inttypes.h>
#include <stdint.h>
#include <errno.h>

static void test_base(void)
{
    int values[] = { 1, 2, 3 };
    NB_CHECK_STR(nb_version(), NB_VERSION_STRING);
    NB_CHECK(NB_ARRAY_SIZE(values) == 3);
}

static void test_err(void)
{
    static const nb_err_desc errs[] = {
        { NB_ERR_USER(1), "TEST_ERR_CONFIG", "config invalid" },
        { NB_ERR_USER(2), "TEST_ERR_AUTH", "authentication failed" },
    };

    nb_err err;
    nb_err_clear(&err);
    NB_CHECK_INT(err.code, NB_OK);
    nb_err_set(&err, NB_ERR_INVAL, "bad %s %d", "value", 7);
    NB_CHECK_INT(err.code, NB_ERR_INVAL);
    NB_CHECK_STR(err.msg, "bad value 7");
    NB_CHECK_STR(nb_status_str(NB_ERR_NOMEM), "out of memory");

    nb_err_set(&err, NB_ERR_TIMEOUT, NULL);
    NB_CHECK_STR(err.msg, "timeout");

    NB_CHECK(NB_ERR_IS_USER(NB_ERR_USER(1)));
    NB_CHECK(!NB_ERR_IS_USER(NB_ERR_INVAL));
    NB_CHECK(NB_ERR_USER(1) != NB_ERR_USER(2));
    NB_CHECK_STR(nb_status_str(NB_ERR_USER(1)), "user error");
    NB_CHECK_STR(nb_status_str((nb_status)-999), "unknown error");

    const nb_err_desc *desc = nb_err_lookup(
        errs, NB_ARRAY_SIZE(errs), NB_ERR_USER(2));
    NB_CHECK(desc != NULL);
    if (desc) {
        NB_CHECK_STR(desc->name, "TEST_ERR_AUTH");
        NB_CHECK_STR(desc->message, "authentication failed");
    }
    NB_CHECK(nb_err_lookup(errs, NB_ARRAY_SIZE(errs), NB_ERR_USER(9)) == NULL);
    NB_CHECK(nb_err_lookup(NULL, NB_ARRAY_SIZE(errs), NB_ERR_USER(1)) == NULL);
    NB_CHECK(nb_err_lookup(errs, 0, NB_ERR_USER(1)) == NULL);

    NB_CHECK_STR(nb_err_str(errs, NB_ARRAY_SIZE(errs), NB_ERR_USER(1)),
                 "config invalid");
    NB_CHECK_STR(nb_err_str(errs, NB_ARRAY_SIZE(errs), NB_ERR_NOMEM),
                 "out of memory");
    NB_CHECK_STR(nb_err_str(NULL, 0, NB_ERR_USER(1)), "user error");

    nb_err_set_os(&err, NB_ERR_OS, 2, "stat failed");
    NB_CHECK_INT(err.os_errno, 2);
    nb_err_set(&err, NB_ERR_IO, "retry");
    NB_CHECK_INT(err.os_errno, 0);
    NB_CHECK_STR(err.msg, "retry");
    NB_CHECK_INT(nb_status_from_errno(ENOENT, &err), NB_ERR_NOT_FOUND);
    NB_CHECK_INT(err.os_errno, ENOENT);
    NB_CHECK_INT(err.code, NB_ERR_NOT_FOUND);
    NB_CHECK_INT(nb_status_from_errno(ECANCELED, NULL), NB_ERR_CANCELLED);
    NB_CHECK_INT(nb_status_from_errno(EACCES, NULL), NB_ERR_PERMISSION);
    NB_CHECK_INT(nb_status_from_errno(0, &err), NB_ERR_IO);
    NB_CHECK_INT(err.os_errno, 0);
    NB_CHECK_INT(nb_err_trace(NULL, NB_ERR_INVAL, NULL, 0, __FILE__, __LINE__), NB_ERR_INVAL);
}

static void test_mem(void)
{
    void *ptr = nb_alloc(128);
    NB_CHECK(ptr != NULL);
    memset(ptr, 0xab, 128);
    nb_free(ptr, 128);

    nb_arena *arena = nb_arena_new(1024);
    NB_CHECK(arena != NULL);
    void *a = nb_arena_alloc(arena, 100);
    void *b = nb_arena_alloc(arena, 100);
    NB_CHECK(a != NULL && b != NULL);
    NB_CHECK(((uintptr_t)a % 16u) == 0u);
    NB_CHECK(nb_arena_used(arena) >= 208u);

    nb_str text = nb_arena_strdup(arena, NB_STR_LIT("arena"));
    NB_CHECK(text.ptr != NULL && text.len == 5u);
    if (text.ptr && text.len == 5u) {
        NB_CHECK_MEM(text.ptr, "arena", 5u);
    }
    unsigned char raw[3] = { 1u, 2u, 3u };
    unsigned char *copy = nb_arena_memdup(arena, raw, sizeof(raw));
    NB_CHECK(copy != NULL);
    if (copy) {
        NB_CHECK_MEM(copy, raw, sizeof(raw));
    }
    NB_CHECK(nb_arena_peak(arena) >= nb_arena_used(arena));
    NB_CHECK_INT(nb_arena_cached(arena),
                 nb_arena_total(arena) - nb_arena_used(arena));

    nb_arena_reset(arena);
    NB_CHECK_INT(nb_arena_used(arena), 0);
    NB_CHECK(nb_arena_peak(arena) > 0);
    NB_CHECK_INT(nb_arena_cached(arena), nb_arena_total(arena));
    nb_arena_free(arena);

    nb_pool *pool = nb_pool_new(sizeof(int), 8);
    NB_CHECK(pool != NULL);
    void *p1 = nb_pool_alloc(pool);
    void *p2 = nb_pool_alloc(pool);
    NB_CHECK(p1 != NULL && p2 != NULL && p1 != p2);
    void *p3 = nb_pool_alloc0(pool);
    NB_CHECK(p3 != NULL && *(int *)p3 == 0);
    NB_CHECK(nb_pool_used(pool) == 3);
    NB_CHECK(nb_pool_peak(pool) >= 3);
    NB_CHECK_INT(nb_pool_cached(pool),
                 nb_pool_capacity(pool) - nb_pool_used(pool));
    nb_pool_release(pool, p1);
    NB_CHECK(nb_pool_used(pool) == 2);
    nb_pool_reset(pool);
    NB_CHECK_INT(nb_pool_used(pool), 0);
    nb_pool_free(pool);

    nb_arena *scoped = nb_arena_new(1024u);
    NB_CHECK(scoped != NULL);
    if (scoped) {
        NB_CHECK(nb_arena_checkpoint(scoped) == NULL);
        void *a = nb_arena_alloc(scoped, 100u);
        NB_CHECK(a != NULL);
        void *mark = nb_arena_checkpoint(scoped);
        NB_CHECK(mark != NULL);
        size_t used = nb_arena_used(scoped);
        void *b = nb_arena_alloc(scoped, 100u);
        NB_CHECK(b != NULL);
        NB_CHECK(nb_arena_used(scoped) > used);

        nb_arena_release(scoped, mark);
        NB_CHECK_INT(nb_arena_used(scoped), used);
        void *c = nb_arena_alloc(scoped, 100u);
        NB_CHECK(c == b);
        nb_arena_release(scoped, mark);
        NB_CHECK_INT(nb_arena_used(scoped), used);

        /* A mark before a new block releases that block as well. */
        void *mark2 = nb_arena_checkpoint(scoped);
        NB_CHECK(mark2 != NULL);
        NB_CHECK(nb_arena_alloc(scoped, 4096u) != NULL);
        size_t total_after = nb_arena_total(scoped);
        nb_arena_release(scoped, mark2);
        NB_CHECK(nb_arena_total(scoped) < total_after);
        NB_CHECK_INT(nb_arena_used(scoped), used);

        nb_arena_release(scoped, NULL);
        NB_CHECK_INT(nb_arena_used(scoped), 0u);
        NB_CHECK_INT(nb_arena_total(scoped), 0u);
        NB_CHECK(nb_arena_checkpoint(scoped) == NULL);
        nb_arena_free(scoped);
    }
}

static void test_str(void)
{
    nb_str hello = NB_STR_LIT("  hello  ");
    NB_CHECK(nb_str_eq(nb_str_trim(hello), NB_STR_LIT("hello")));
    NB_CHECK(nb_str_eq_cstr(NB_STR_LIT("abc"), "abc"));
    NB_CHECK(nb_str_starts_with(NB_STR_LIT("abcdef"), NB_STR_LIT("abc")));
    NB_CHECK(nb_str_ends_with(NB_STR_LIT("abcdef"), NB_STR_LIT("def")));
    NB_CHECK_INT(nb_str_find(NB_STR_LIT("abcdef"), NB_STR_LIT("cd")), 2);
    NB_CHECK_INT(nb_str_find(NB_STR_LIT("abcdef"), NB_STR_LIT("zz")), SIZE_MAX);

    int64_t i = 0;
    NB_CHECK(nb_str_to_i64(NB_STR_LIT(" -9223372036854775808 "), &i));
    NB_CHECK(i == INT64_MIN);
    NB_CHECK(!nb_str_to_i64(NB_STR_LIT("9223372036854775808"), &i));
    NB_CHECK(!nb_str_to_i64(NB_STR_LIT("12x"), &i));

    uint64_t u = 0;
    NB_CHECK(nb_str_to_u64(NB_STR_LIT("18446744073709551615"), &u));
    NB_CHECK(u == UINT64_MAX);
    NB_CHECK(!nb_str_to_u64(NB_STR_LIT("-1"), &u));

    double d = 0;
    NB_CHECK(nb_str_to_f64(NB_STR_LIT(" 3.5 "), &d));
    NB_CHECK(d == 3.5);
    NB_CHECK(!nb_str_to_f64(NB_STR_LIT("3.5x"), &d));

    /* A length-bearing view with a NULL pointer is reachable through the
     * public constructor; operations must not dereference it. */
    nb_str invalid = nb_str_from_parts(NULL, 3u);
    NB_CHECK_INT(nb_str_cmp(invalid, NB_STR_NULL), 0);
    NB_CHECK(!nb_str_starts_with(invalid, NB_STR_LIT("a")));
    NB_CHECK(!nb_str_ends_with(invalid, NB_STR_LIT("a")));
    NB_CHECK_INT(nb_str_trim(invalid).len, 0u);
    NB_CHECK(!nb_str_to_i64(invalid, &i));
    NB_CHECK(!nb_str_to_u64(invalid, &u));
    NB_CHECK(!nb_str_to_f64(invalid, &d));
    NB_CHECK_INT(nb_hash_bytes(NULL, 3u), 0u);
    NB_CHECK_INT(nb_str_hash(invalid), 0u);

    NB_CHECK(nb_str_hash(NB_STR_LIT("abc")) ==
             nb_hash_bytes("abc", 3));
}

static void test_buf(void)
{
    nb_buf *buf = nb_buf_new();
    NB_CHECK(buf != NULL);
    NB_CHECK(nb_buf_append_cstr(buf, "hello"));
    NB_CHECK(nb_buf_append_char(buf, ' '));
    NB_CHECK(nb_buf_printf(buf, "%d", 42));
    NB_CHECK_STR(nb_buf_cstr(buf), "hello 42");
    NB_CHECK_INT(nb_buf_len(buf), 8);
    nb_buf_consume(buf, 6);
    NB_CHECK_STR(nb_buf_cstr(buf), "42");
    NB_CHECK(nb_buf_append_str(buf, NB_STR_LIT("!")));
    NB_CHECK_STR(nb_buf_cstr(buf), "42!");
    nb_buf_clear(buf);
    NB_CHECK_STR(nb_buf_cstr(buf), "");
    nb_buf_free(buf);
}

static void test_vec(void)
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
    NB_CHECK_INT(nb_vec_len(&vec), 100);
    for (int i = 0; i < 100; i++) {
        int *slot = nb_vec_at(&vec, (size_t)i);
        NB_CHECK(slot != NULL && *slot == i);
    }
    nb_vec_pop(&vec);
    NB_CHECK_INT(nb_vec_len(&vec), 99);
    nb_vec_clear(&vec);
    NB_CHECK(nb_vec_empty(&vec));
    nb_vec_destroy(&vec);
}

static void test_map(void)
{
    nb_map *map = nb_map_new();
    char key[32];
    for (int i = 0; i < 200; i++) {
        snprintf(key, sizeof(key), "key-%d", i);
        NB_CHECK(nb_map_put_cstr(map, key, (void *)(intptr_t)(i + 1), NULL));
    }
    NB_CHECK_INT(nb_map_size(map), 200);
    NB_CHECK_INT((intptr_t)nb_map_get_cstr(map, "key-42"), 43);
    NB_CHECK(nb_map_has_cstr(map, "key-199"));
    NB_CHECK(!nb_map_has_cstr(map, "missing"));
    NB_CHECK(nb_map_put_cstr(map, "key-42", (void *)(intptr_t)777, NULL));
    NB_CHECK_INT((intptr_t)nb_map_get_cstr(map, "key-42"), 777);
    NB_CHECK_INT(nb_map_size(map), 200);

    nb_map_iter it;
    nb_map_iter_init(map, &it);
    nb_str k;
    void *v;
    int count = 0;
    int order = 0;
    while (nb_map_iter_next(&it, &k, &v)) {
        char expected[32];
        snprintf(expected, sizeof(expected), "key-%d", order);
        NB_CHECK(nb_str_eq_cstr(k, expected));
        order++;
        count++;
        (void)v;
    }
    NB_CHECK_INT(count, 200);

    NB_CHECK_INT((intptr_t)nb_map_del_cstr(map, "key-42"), 777);
    NB_CHECK(!nb_map_has_cstr(map, "key-42"));
    NB_CHECK_INT(nb_map_size(map), 199);
    nb_map_clear(map);
    NB_CHECK_INT(nb_map_size(map), 0);
    nb_map_free(map);
}

static void test_time(void)
{
    int64_t ns = 0;
    NB_CHECK(nb_time_parse_rfc3339("1970-01-01T00:00:00Z", &ns));
    NB_CHECK_INT(ns, 0);
    NB_CHECK(nb_time_parse_rfc3339("1970-01-01T08:00:00+08:00", &ns));
    NB_CHECK_INT(ns, 0);
    NB_CHECK(nb_time_parse_rfc3339("2026-11-09T08:14:00.123Z", &ns));
    NB_CHECK_INT(ns, 1794212040123000000LL);
    NB_CHECK(nb_time_parse_rfc3339_str(
        nb_str_from_parts("1970-01-01T00:00:00Z", 20u), &ns));
    NB_CHECK_INT(ns, 0);
    NB_CHECK(!nb_time_parse_rfc3339_str(
        NB_STR_LIT("1970-01-01T00:00:00"), &ns));
    NB_CHECK(!nb_time_parse_rfc3339("2026-13-01T00:00:00Z", &ns));
    NB_CHECK(!nb_time_parse_rfc3339("2026-01-01T00:00:00", &ns));

    int64_t value = nb_time_now_ns();
    char text[64];
    NB_CHECK(nb_time_format_rfc3339(value, text, sizeof(text)));
    int64_t parsed = 0;
    NB_CHECK(nb_time_parse_rfc3339(text, &parsed));
    NB_CHECK((parsed / 1000000) == (value / 1000000));
    NB_CHECK(nb_time_monotonic_ns() > 0);
}

typedef struct log_sink_state {
    int count;
    char last[512];
} log_sink_state;

static void capture_sink(void *ctx, nb_log_level level,
                         const char *record, size_t len)
{
    (void)level;
    log_sink_state *state = ctx;
    state->count++;
    size_t n = len < sizeof(state->last) - 1u ? len : sizeof(state->last) - 1u;
    memcpy(state->last, record, n);
    state->last[n] = '\0';
}

static void test_log(void)
{
    log_sink_state state;
    memset(&state, 0, sizeof(state));
    nb_log *log = nb_log_new(NB_LOG_INFO, capture_sink, &state);
    NB_CHECK(log != NULL);
    nb_log_write(log, NB_LOG_DEBUG, "hidden");
    nb_log_write(log, NB_LOG_INFO, "hello %d", 1);
    nb_log_write(log, NB_LOG_WARN, "warn");
    NB_CHECK_INT(state.count, 2);
    NB_CHECK(strstr(state.last, "[WARN]") != NULL);
    NB_CHECK(strstr(state.last, "warn") != NULL);
    nb_log_set_level(log, NB_LOG_ERROR);
    nb_log_write(log, NB_LOG_WARN, "hidden again");
    NB_CHECK_INT(state.count, 2);
    bool ok = false;
    NB_CHECK_INT(nb_log_level_from_str("debug", &ok), NB_LOG_DEBUG);
    NB_CHECK(ok);
    nb_log_free(log);
}

void test_core(void)
{
    test_base();
    test_err();
    test_mem();
    test_str();
    test_buf();
    test_vec();
    test_map();
    test_time();
    test_log();
}
