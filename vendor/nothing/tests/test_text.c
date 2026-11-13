#include "test.h"

#include "nb/nbase.h"

static int map_dtor_count;

static void count_map_value(void *value, void *ctx)
{
    (void)value;
    (*(int *)ctx)++;
}

static void test_str_icase_and_find(void)
{
    NB_CHECK(nb_str_eq_icase(NB_STR_LIT("AbC"), NB_STR_LIT("aBc")));
    NB_CHECK(!nb_str_eq_icase(NB_STR_LIT("abc"), NB_STR_LIT("ab")));
    NB_CHECK(nb_str_eq_icase(NB_STR_NULL, NB_STR_NULL));
    NB_CHECK(!nb_str_eq_icase(NB_STR_LIT("\xC0"), NB_STR_LIT("\xE0")));
    NB_CHECK(!nb_str_eq_icase(NB_STR_LIT("a"), NB_STR_NULL));

    NB_CHECK_INT(nb_str_find_char(NB_STR_LIT("hello"), 'l'), 2);
    NB_CHECK_INT(nb_str_find_char(NB_STR_LIT("hello"), 'z'), SIZE_MAX);
    NB_CHECK_INT(nb_str_find_char(NB_STR_NULL, 'a'), SIZE_MAX);
    NB_CHECK_INT(nb_str_find_char(nb_str_from_parts("a\0b", 3), '\0'), 1);

    NB_CHECK_INT(nb_str_find(NB_STR_LIT("aaab"), NB_STR_LIT("aab")), 1);
    NB_CHECK_INT(nb_str_find(NB_STR_LIT("aaa"), NB_STR_LIT("")), 0);
    NB_CHECK_INT(nb_str_find(NB_STR_LIT("abc"), NB_STR_LIT("abcd")), SIZE_MAX);
    NB_CHECK_INT(nb_str_find(NB_STR_LIT("abcabc"), NB_STR_LIT("cab")), 2);
    NB_CHECK_INT(nb_str_find(NB_STR_NULL, NB_STR_LIT("x")), SIZE_MAX);
}

static void check_split(const char *input, char delim,
                        const char *const *expected, size_t count)
{
    nb_split_iter it;
    nb_split_init(&it, nb_str_from_cstr(input), delim);
    for (size_t i = 0; i < count; i++) {
        nb_str token;
        NB_CHECK(nb_split_next(&it, &token));
        size_t want = strlen(expected[i]);
        NB_CHECK_INT(token.len, want);
        if (token.len == want && want != 0) {
            NB_CHECK_MEM(token.ptr, expected[i], want);
        }
    }
    nb_str extra;
    NB_CHECK(!nb_split_next(&it, &extra));
}

static void test_str_split(void)
{
    static const char *const empty[] = { "" };
    static const char *const middle[] = { "a", "", "b" };
    static const char *const leading[] = { "", "a" };
    static const char *const trailing[] = { "a", "" };
    static const char *const plain[] = { "abc" };
    static const char *const semis[] = { "a", "b", "c" };

    check_split("", ',', empty, NB_ARRAY_SIZE(empty));
    check_split("a,,b", ',', middle, NB_ARRAY_SIZE(middle));
    check_split(",a", ',', leading, NB_ARRAY_SIZE(leading));
    check_split("a,", ',', trailing, NB_ARRAY_SIZE(trailing));
    check_split("abc", ',', plain, NB_ARRAY_SIZE(plain));
    check_split("a;b;c", ';', semis, NB_ARRAY_SIZE(semis));

    nb_split_iter it;
    nb_split_init(&it, NB_STR_NULL, ',');
    nb_str token;
    NB_CHECK(nb_split_next(&it, &token));
    NB_CHECK_INT(token.len, 0);
    NB_CHECK(!nb_split_next(&it, &token));
    NB_CHECK(!nb_split_next(NULL, &token));
}

static void test_split_skip_empty(void)
{
    nb_split_iter it;
    nb_str token;

    nb_split_init(&it, NB_STR_LIT("  a   b "), ' ');
    nb_split_skip_empty(&it, true);
    NB_CHECK(nb_split_next(&it, &token));
    NB_CHECK_INT(token.len, 1u);
    NB_CHECK_MEM(token.ptr, "a", 1u);
    NB_CHECK(nb_split_next(&it, &token));
    NB_CHECK_INT(token.len, 1u);
    NB_CHECK_MEM(token.ptr, "b", 1u);
    NB_CHECK(!nb_split_next(&it, &token));

    nb_split_init(&it, NB_STR_NULL, ',');
    nb_split_skip_empty(&it, true);
    NB_CHECK(!nb_split_next(&it, &token));
}

static void test_map_update_and_order(void)
{
    nb_map *map = nb_map_new();
    NB_CHECK(map != NULL);
    if (!map) {
        return;
    }

    void *old = (void *)(intptr_t)1;
    NB_CHECK(nb_map_put(map, NB_STR_LIT("alice"), (void *)(intptr_t)10, &old));
    NB_CHECK(old == NULL);
    NB_CHECK(nb_map_put(map, NB_STR_LIT("bob"), (void *)(intptr_t)20, &old));
    NB_CHECK(old == NULL);
    NB_CHECK(nb_map_put(map, NB_STR_LIT("carol"), (void *)(intptr_t)30, &old));
    NB_CHECK(old == NULL);

    NB_CHECK(nb_map_put(map, NB_STR_LIT("bob"), (void *)(intptr_t)22, &old));
    NB_CHECK(old == (void *)(intptr_t)20);
    NB_CHECK(nb_map_get_cstr(map, "bob") == (void *)(intptr_t)22);
    NB_CHECK_INT(nb_map_size(map), 3);

    old = (void *)(intptr_t)1;
    NB_CHECK(nb_map_put_cstr(map, "alice", (void *)(intptr_t)11, &old));
    NB_CHECK(old == (void *)(intptr_t)10);

    static const char *const order[] = { "alice", "bob", "carol" };
    nb_map_iter it;
    nb_map_iter_init(map, &it);
    nb_str key;
    void *value;
    size_t i = 0;
    while (nb_map_iter_next(&it, &key, &value)) {
        NB_CHECK(i < NB_ARRAY_SIZE(order));
        if (i < NB_ARRAY_SIZE(order)) {
            NB_CHECK_INT(key.len, strlen(order[i]));
            if (key.len == strlen(order[i])) {
                NB_CHECK_MEM(key.ptr, order[i], key.len);
            }
        }
        i++;
    }
    NB_CHECK_INT(i, 3);

    size_t old_cap = nb_map_capacity(map);
    NB_CHECK(nb_map_reserve(map, 64));
    NB_CHECK(nb_map_capacity(map) >= 64);
    NB_CHECK(nb_map_capacity(map) > old_cap);
    NB_CHECK(nb_map_get_cstr(map, "carol") == (void *)(intptr_t)30);

    old_cap = nb_map_capacity(map);
    NB_CHECK(!nb_map_reserve(map, SIZE_MAX));
    NB_CHECK_INT(nb_map_capacity(map), old_cap);

    NB_CHECK(nb_map_del_cstr(map, "bob") == (void *)(intptr_t)22);
    NB_CHECK(!nb_map_has_cstr(map, "bob"));
    NB_CHECK_INT(nb_map_size(map), 2);
    nb_map_free(map);
}

static void test_map_edge_cases(void)
{
    nb_map *map = nb_map_new_with_cap(0);
    NB_CHECK(map != NULL);
    if (!map) {
        return;
    }
    NB_CHECK(nb_map_put(map, NB_STR_NULL, (void *)(intptr_t)1, NULL));
    NB_CHECK(nb_map_has(map, NB_STR_NULL));
    NB_CHECK(nb_map_get(map, NB_STR_NULL) == (void *)(intptr_t)1);
    NB_CHECK(nb_map_reserve(map, 8));
    NB_CHECK(nb_map_del(map, NB_STR_NULL) == (void *)(intptr_t)1);
    NB_CHECK(!nb_map_put(NULL, NB_STR_LIT("x"), NULL, NULL));
    NB_CHECK(!nb_map_put(map, nb_str_from_parts(NULL, 1), NULL, NULL));
    NB_CHECK(!nb_map_reserve(NULL, 8));
    nb_map_free(map);
}

static void test_map_reserve_capacity(void)
{
    nb_map *map = nb_map_new();
    NB_CHECK(map != NULL);
    if (!map) {
        return;
    }
    NB_CHECK(nb_map_reserve(map, 64u));
    size_t capacity = nb_map_capacity(map);
    char key[32];
    for (size_t i = 0; i < 64u; i++) {
        snprintf(key, sizeof(key), "reserve-%zu", i);
        NB_CHECK(nb_map_put_cstr(map, key, (void *)(uintptr_t)(i + 1u), NULL));
    }
    /* Reserve promises room for 64 entries without a rehash; the bucket
     * count must not change when the last one is inserted. */
    NB_CHECK_INT(nb_map_capacity(map), capacity);
    NB_CHECK_INT(nb_map_size(map), 64u);
    nb_map_free(map);

    map = nb_map_new();
    NB_CHECK(map != NULL);
    if (map) {
        NB_CHECK(nb_map_reserve(map, 7u));
        NB_CHECK(nb_map_capacity(map) >= 16u);
        nb_map_free(map);
    }
}

static void test_map_entry_reuse(void)
{
    nb_map *map = nb_map_new();
    NB_CHECK(map != NULL);
    if (!map) {
        return;
    }

    char key[128];
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 200; i++) {
            snprintf(key, sizeof(key), "round-%d-key-%03d", round, i);
            NB_CHECK(nb_map_put_cstr(map, key, (void *)(intptr_t)(i + 1),
                                     NULL));
        }
        for (int i = 0; i < 200; i += 2) {
            snprintf(key, sizeof(key), "round-%d-key-%03d", round, i);
            NB_CHECK(nb_map_del_cstr(map, key) == (void *)(intptr_t)(i + 1));
        }
        for (int i = 0; i < 200; i += 2) {
            snprintf(key, sizeof(key), "round-%d-key-%03d", round, i);
            NB_CHECK(nb_map_put_cstr(map, key, (void *)(intptr_t)(i + 1),
                                     NULL));
        }
        NB_CHECK_INT(nb_map_size(map), 200u);
        nb_map_clear(map);
        NB_CHECK_INT(nb_map_size(map), 0u);
    }

    /* Keys above the slab bin cap take the direct-allocation path. */
    char large[5000];
    memset(large, 'x', sizeof(large) - 1u);
    large[sizeof(large) - 1u] = '\0';
    NB_CHECK(nb_map_put_cstr(map, large, (void *)(intptr_t)7, NULL));
    NB_CHECK(nb_map_get_cstr(map, large) == (void *)(intptr_t)7);
    NB_CHECK(nb_map_del_cstr(map, large) == (void *)(intptr_t)7);
    nb_map_free(map);
}

static void test_map_slot_and_dtor(void)
{
    nb_map *map = nb_map_new();
    NB_CHECK(map != NULL);
    if (!map) {
        return;
    }

    bool created = false;
    void **slot = nb_map_slot(map, NB_STR_LIT("x"), &created);
    NB_CHECK(slot != NULL && created && *slot == NULL);
    if (slot) {
        *slot = (void *)(intptr_t)7;
    }
    slot = nb_map_slot(map, NB_STR_LIT("x"), &created);
    NB_CHECK(slot != NULL && !created);
    NB_CHECK(slot != NULL && *slot == (void *)(intptr_t)7);
    NB_CHECK_INT(nb_map_size(map), 1);

    map_dtor_count = 0;
    nb_map_set_value_dtor(map, count_map_value, &map_dtor_count);
    NB_CHECK(nb_map_put_cstr(map, "y", (void *)(intptr_t)8, NULL));
    nb_map_clear(map);
    NB_CHECK_INT(map_dtor_count, 2);
    NB_CHECK_INT(nb_map_size(map), 0);

    NB_CHECK(nb_map_put_cstr(map, "z", (void *)(intptr_t)9, NULL));
    nb_map_free(map);
    NB_CHECK_INT(map_dtor_count, 3);
}

void test_text(void)
{
    test_str_icase_and_find();
    test_str_split();
    test_split_skip_empty();
    test_map_update_and_order();
    test_map_edge_cases();
    test_map_reserve_capacity();
    test_map_entry_reuse();
    test_map_slot_and_dtor();
}
