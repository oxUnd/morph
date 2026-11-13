#include "test.h"

#include "nb/nbase.h"

/* Always move reallocations and verify sized frees. This makes stale source
 * pointers deterministic without relying on a particular system allocator. */
typedef union checked_header {
    max_align_t alignment;
    size_t size;
} checked_header;

typedef struct checked_allocator {
    size_t calls;
    size_t fail_at;
    size_t live;
    const char *grow_path;
    bool grew;
} checked_allocator;

static void *checked_alloc(void *ctx, size_t size)
{
    checked_allocator *state = ctx;
    if (++state->calls == state->fail_at || size > SIZE_MAX - sizeof(checked_header)) {
        return NULL;
    }
    if (state->grow_path && !state->grew && size >= 65536u) {
        state->grew = true;
        FILE *file = fopen(state->grow_path, "ab");
        NB_CHECK(file != NULL);
        if (file) {
            NB_CHECK_INT(fwrite("x", 1u, 1u, file), 1u);
            NB_CHECK_INT(fclose(file), 0);
        }
    }
    checked_header *header = malloc(sizeof(*header) + size);
    if (!header) {
        return NULL;
    }
    header->size = size;
    state->live++;
    return header + 1;
}

static void checked_free(void *ctx, void *ptr, size_t size)
{
    checked_allocator *state = ctx;
    if (ptr) {
        checked_header *header = (checked_header *)ptr - 1;
        NB_CHECK_INT(header->size, size);
        NB_CHECK(state->live > 0);
        state->live--;
        free(header);
    }
}

static void *checked_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size)
{
    void *result = checked_alloc(ctx, new_size);
    if (result) {
        memcpy(result, ptr, NB_MIN(old_size, new_size));
        checked_free(ctx, ptr, old_size);
    }
    return result;
}

static void test_self_copy(void)
{
    nb_buf buf;
    nb_buf_init(&buf);
    NB_CHECK(nb_buf_append_cstr(&buf, "abcdefghijklmno"));
    NB_CHECK(nb_buf_append_str(&buf, nb_buf_str(&buf)));
    NB_CHECK_STR(nb_buf_cstr(&buf), "abcdefghijklmnoabcdefghijklmno");
    nb_buf_clear(&buf);
    NB_CHECK(nb_buf_append_cstr(&buf, "0123456789"));
    nb_buf_consume(&buf, 8u);
    const char *consumed = buf.ptr + 1u;
    NB_CHECK(nb_buf_append(&buf, consumed, 8u));
    NB_CHECK_STR(nb_buf_cstr(&buf), "8912345678");
    NB_CHECK(nb_buf_printf(&buf, "%s", nb_buf_cstr(&buf)));
    NB_CHECK_STR(nb_buf_cstr(&buf), "89123456788912345678");
    nb_buf_clear(&buf);
    NB_CHECK(nb_buf_append_cstr(&buf, "%s"));
    NB_CHECK(nb_buf_printf(&buf, nb_buf_cstr(&buf), "formatted"));
    NB_CHECK_STR(nb_buf_cstr(&buf), "%sformatted");
    nb_buf_clear(&buf);
    char long_text[600];
    memset(long_text, 'q', sizeof(long_text));
    NB_CHECK(nb_buf_append(&buf, long_text, sizeof(long_text)));
    NB_CHECK(nb_buf_printf(&buf, "%s", nb_buf_cstr(&buf)));
    NB_CHECK_INT(nb_buf_len(&buf), 1200u);
    for (size_t i = 0; i < nb_buf_len(&buf); i++) {
        NB_CHECK(buf.ptr[i] == 'q');
    }
    nb_buf_destroy(&buf);

    for (size_t source = 0; source < 8u; source++) {
        for (size_t index = 0; index <= 8u; index++) {
            nb_vec vec;
            nb_vec_init(&vec, sizeof(int));
            for (int value = 0; value < 8; value++) {
                NB_CHECK(nb_vec_push_copy(&vec, &value));
            }
            NB_CHECK(nb_vec_insert(&vec, index, nb_vec_at(&vec, source)));
            for (size_t i = 0; i < 9u; i++) {
                int expected = i == index ? (int)source : (int)(i - (i > index));
                NB_CHECK_INT(*(int *)nb_vec_at(&vec, i), expected);
            }
            nb_vec_destroy(&vec);
        }
    }
    nb_vec vec;
    nb_vec_init(&vec, sizeof(int));
    for (int value = 0; value < 8; value++) {
        NB_CHECK(nb_vec_push_copy(&vec, &value));
    }
    NB_CHECK(nb_vec_push_copy(&vec, nb_vec_at(&vec, 3u)));
    NB_CHECK_INT(*(int *)nb_vec_at(&vec, 8u), 3);
    nb_vec_destroy(&vec);
    nb_vec forged = { NULL, SIZE_MAX, SIZE_MAX, 1u };
    NB_CHECK(nb_vec_push(&forged) == NULL);
    NB_CHECK_INT(forged.len, SIZE_MAX);
}

static void test_arena_reuse(void)
{
    nb_arena *arena = nb_arena_new(1024u);
    NB_CHECK(arena != NULL);
    if (!arena) {
        return;
    }
    size_t total = 0;
    for (size_t phase = 0; phase < 20u; phase++) {
        for (size_t i = 0; i < 100u; i++) {
            unsigned char *data = nb_arena_alloc(arena, 1024u);
            NB_CHECK(data != NULL);
            if (data) {
                memset(data, (int)i, 1024u);
            }
        }
        if (phase == 0) {
            total = nb_arena_total(arena);
        }
        NB_CHECK_INT(nb_arena_total(arena), total);
        NB_CHECK_INT(nb_arena_used(arena), 102400u);
        nb_arena_reset(arena);
    }
    /* A checkpoint inside a reused block must preserve all earlier blocks. */
    unsigned char *first = nb_arena_alloc(arena, 1024u);
    NB_CHECK(first != NULL);
    if (first) {
        memset(first, 0xa5, 1024u);
    }
    for (size_t i = 0; i < 10u; i++) {
        NB_CHECK(nb_arena_alloc(arena, 1024u) != NULL);
    }
    void *outer = nb_arena_checkpoint(arena);
    size_t used = nb_arena_used(arena);
    NB_CHECK(nb_arena_alloc(arena, 20000u) != NULL);
    void *inner = nb_arena_checkpoint(arena);
    NB_CHECK(nb_arena_alloc(arena, 80000u) != NULL);
    nb_arena_release(arena, inner);
    nb_arena_release(arena, outer);
    NB_CHECK_INT(nb_arena_used(arena), used);
    if (first) {
        for (size_t i = 0; i < 1024u; i++) {
            NB_CHECK_INT(first[i], 0xa5);
        }
    }
    NB_CHECK(nb_arena_alloc(arena, 1024u) == outer);
    nb_arena_release(arena, NULL);
    NB_CHECK_INT(nb_arena_total(arena), 0u);
    nb_arena_free(arena);
}

static void test_encoding_self_input(void)
{
    static const char *inputs[] = { "hello world!", "aGVsbG8=", "01aB", "a b/c", "%61+b" };
    static const char *outputs[] = { "aGVsbG8gd29ybGQh", "hello", "\x01\xab", "a+b%2Fc", "a b" };
    for (unsigned op = 0; op < 6u; op++) {
        nb_buf buf;
        nb_buf_init(&buf);
        unsigned which = op == 5u ? 0u : op;
        NB_CHECK(nb_buf_append_cstr(&buf, "discarded-prefix/"));
        NB_CHECK(nb_buf_append_cstr(&buf, inputs[which]));
        nb_buf_consume(&buf, 17u);
        nb_str input = nb_buf_str(&buf);
        nb_status status;
        switch (op) {
        case 0: status = nb_base64_encode(input.ptr, input.len, NB_B64_STD, true, &buf, NULL); break;
        case 1: status = nb_base64_decode(input, NB_B64_STD, &buf, NULL); break;
        case 2: status = nb_hex_decode(input, &buf, NULL); break;
        case 3: status = nb_url_encode(input, true, &buf, NULL); break;
        case 4: status = nb_url_decode(input, true, &buf, NULL); break;
        default: status = nb_hex_encode(input.ptr, input.len, false, &buf, NULL); break;
        }
        const char *expected = op == 5u ? "68656c6c6f20776f726c6421" : outputs[which];
        NB_CHECK_INT(status, NB_OK);
        NB_CHECK_INT(nb_buf_len(&buf), strlen(inputs[which]) + strlen(expected));
        NB_CHECK_MEM(nb_buf_cstr(&buf), inputs[which], strlen(inputs[which]));
        NB_CHECK_MEM(nb_buf_cstr(&buf) + strlen(inputs[which]), expected, strlen(expected));
        nb_buf_destroy(&buf);
    }

    nb_buf encoded;
    nb_buf decoded;
    nb_buf_init(&encoded);
    nb_buf_init(&decoded);
    unsigned char bytes[1024];
    for (size_t i = 0; i < sizeof(bytes); i++) {
        bytes[i] = (unsigned char)(i * 73u + 19u);
    }
    for (size_t len = 0; len <= sizeof(bytes); len++) {
        for (unsigned variant = 0; variant < 2u; variant++) {
            for (unsigned pad = 0; pad < 2u; pad++) {
                nb_buf_clear(&encoded);
                nb_buf_clear(&decoded);
                NB_CHECK_INT(nb_base64_encode(bytes, len, (nb_b64_variant)variant,
                                              pad != 0, &encoded, NULL), NB_OK);
                NB_CHECK(nb_base64_valid(nb_buf_str(&encoded), (nb_b64_variant)variant));
                NB_CHECK_INT(nb_base64_decode(nb_buf_str(&encoded), (nb_b64_variant)variant,
                                              &decoded, NULL), NB_OK);
                NB_CHECK_INT(nb_buf_len(&decoded), len);
                NB_CHECK_MEM(nb_buf_cstr(&decoded), bytes, len);
            }
        }
        nb_buf_clear(&encoded);
        nb_buf_clear(&decoded);
        NB_CHECK_INT(nb_hex_encode(bytes, len, len % 2u != 0u, &encoded, NULL), NB_OK);
        NB_CHECK_INT(nb_hex_decode(nb_buf_str(&encoded), &decoded, NULL), NB_OK);
        NB_CHECK_INT(nb_buf_len(&decoded), len);
        NB_CHECK_MEM(nb_buf_cstr(&decoded), bytes, len);
        nb_buf_clear(&encoded);
        nb_buf_clear(&decoded);
        NB_CHECK_INT(nb_url_encode(nb_str_from_parts((const char *)bytes, len),
                                  len % 2u != 0u, &encoded, NULL), NB_OK);
        NB_CHECK_INT(nb_url_decode(nb_buf_str(&encoded), len % 2u != 0u,
                                  &decoded, NULL), NB_OK);
        NB_CHECK_INT(nb_buf_len(&decoded), len);
        NB_CHECK_MEM(nb_buf_cstr(&decoded), bytes, len);
    }
    nb_buf_destroy(&encoded);
    nb_buf_destroy(&decoded);
}

static void test_failure_atomicity(checked_allocator *state)
{
    nb_buf buf;
    nb_buf_init(&buf);
    NB_CHECK(nb_buf_append_cstr(&buf, "discard-this/keep"));
    nb_buf_consume(&buf, 13u);
    nb_buf before = buf;
    state->fail_at = state->calls + 1u;
    NB_CHECK(!nb_buf_shrink(&buf));
    NB_CHECK(buf.ptr == before.ptr);
    NB_CHECK_INT(buf.off, before.off);
    NB_CHECK_INT(buf.len, before.len);
    NB_CHECK_INT(buf.cap, before.cap);
    NB_CHECK_STR(buf.ptr, "discard-this/keep");
    state->fail_at = 0;
    NB_CHECK_INT(nb_hex_decode(NB_STR_LIT("00xz"), &buf, NULL), NB_ERR_PARSE);
    NB_CHECK_INT(buf.off, before.off);
    NB_CHECK_STR(nb_buf_cstr(&buf), "keep");
    NB_CHECK_INT(nb_url_decode(NB_STR_LIT("ok%"), false, &buf, NULL), NB_ERR_PARSE);
    NB_CHECK_INT(buf.off, before.off);
    NB_CHECK_INT(nb_base64_encode("x", SIZE_MAX, NB_B64_STD, true, &buf, NULL), NB_ERR_RANGE);
    NB_CHECK_INT(nb_hex_encode("x", SIZE_MAX, true, &buf, NULL), NB_ERR_RANGE);
    NB_CHECK_INT(nb_path_clean(&buf, ""), NB_ERR_INVAL);
    NB_CHECK_STR(nb_buf_cstr(&buf), "keep");
    state->fail_at = state->calls + 1u;
    NB_CHECK_INT(nb_path_join(&buf, "parent", "child"), NB_ERR_NOMEM);
    NB_CHECK_STR(nb_buf_cstr(&buf), "keep");
    state->fail_at = 0;
    nb_buf_destroy(&buf);

    bool reached_success = false;
    for (size_t failure = 1; failure < 20u && !reached_success; failure++) {
        nb_ini *ini = nb_ini_new();
        NB_CHECK(ini != NULL);
        if (!ini) {
            break;
        }
        NB_CHECK_INT(nb_ini_set(ini, "existing", "key", "old"), NB_OK);
        state->fail_at = state->calls + failure;
        nb_status status = nb_ini_set(ini, "new-section", "key", "value");
        state->fail_at = 0;
        NB_CHECK_STR(nb_ini_get(ini, "existing", "key"), "old");
        if (status == NB_OK) {
            reached_success = true;
            NB_CHECK_INT(nb_ini_section_count(ini), 2u);
        } else {
            NB_CHECK_INT(status, NB_ERR_NOMEM);
            NB_CHECK_INT(nb_ini_section_count(ini), 1u);
            NB_CHECK(!nb_ini_has(ini, "new-section", "key"));
        }
        nb_ini_free(ini);
    }
    NB_CHECK(reached_success);
    state->fail_at = state->calls + 1u;
    NB_CHECK(nb_log_new(NB_LOG_INFO, NULL, NULL) == NULL);
    state->fail_at = state->calls + 2u;
    NB_CHECK(nb_log_new(NB_LOG_INFO, NULL, NULL) == NULL);
    state->fail_at = 0;
}

static void test_ini_transactions(checked_allocator *state)
{
    bool success = false;
    for (size_t failure = 1; failure < 100u && !success; failure++) {
        nb_ini *ini = nb_ini_from_string(NB_STR_LIT("[existing]\nkey = old\nbinary = abc\n"), NULL);
        NB_CHECK(ini != NULL);
        if (!ini) {
            return;
        }
        nb_err err;
        nb_err_clear(&err);
        state->fail_at = state->calls + failure;
        nb_status status = nb_ini_parse(ini, NB_STR_LIT("[existing]\nkey = new\n[new]\nx = value\n"), &err);
        state->fail_at = 0;
        nb_str binary = nb_ini_get_str(ini, "existing", "binary");
        NB_CHECK_INT(binary.len, 3u);
        if (binary.len == 3u) {
            NB_CHECK_MEM(binary.ptr, "abc", 3u);
        }
        if (status == NB_OK) {
            success = true;
            NB_CHECK_STR(nb_ini_get(ini, "existing", "key"), "new");
            NB_CHECK_STR(nb_ini_get(ini, "new", "x"), "value");
        } else {
            NB_CHECK_INT(status, NB_ERR_NOMEM);
            NB_CHECK_INT(err.code, status);
            NB_CHECK_STR(nb_ini_get(ini, "existing", "key"), "old");
            NB_CHECK_INT(nb_ini_section_count(ini), 1u);
        }
        nb_ini_free(ini);
    }
    NB_CHECK(success);

    for (unsigned op = 0; op < 6u; op++) {
        for (size_t failure = 1; failure <= 2u; failure++) {
            nb_buf buf;
            nb_buf_init(&buf);
            const char *input = op == 1u ? "aGVsbG8gd29ybGQ=" :
                                op == 2u ? "68656c6c6f776f72" :
                                op == 4u ? "%61%62%63%64%65" : "hello world!!!!";
            NB_CHECK(nb_buf_append_cstr(&buf, input));
            NB_CHECK(nb_buf_shrink(&buf));
            nb_buf before = buf;
            nb_str view = nb_buf_str(&buf);
            state->fail_at = state->calls + failure;
            nb_status status;
            switch (op) {
            case 0: status = nb_base64_encode(view.ptr, view.len, NB_B64_STD, true, &buf, NULL); break;
            case 1: status = nb_base64_decode(view, NB_B64_STD, &buf, NULL); break;
            case 2: status = nb_hex_decode(view, &buf, NULL); break;
            case 3: status = nb_url_encode(view, true, &buf, NULL); break;
            case 4: status = nb_url_decode(view, true, &buf, NULL); break;
            default: status = nb_hex_encode(view.ptr, view.len, false, &buf, NULL); break;
            }
            state->fail_at = 0;
            NB_CHECK_INT(status, NB_ERR_NOMEM);
            NB_CHECK(buf.ptr == before.ptr);
            NB_CHECK_INT(buf.len, before.len);
            NB_CHECK_INT(buf.off, before.off);
            NB_CHECK_INT(buf.cap, before.cap);
            NB_CHECK_STR(nb_buf_cstr(&buf), input);
            nb_buf_destroy(&buf);
        }
    }
    nb_ini *ini = nb_ini_from_string(NB_STR_LIT("key = value\n"), NULL);
    NB_CHECK(ini != NULL);
    if (ini) {
        success = false;
        for (size_t failure = 1; failure < 20u && !success; failure++) {
            nb_buf buf;
            nb_buf_init(&buf);
            NB_CHECK(nb_buf_append_cstr(&buf, "discard-prefix/keep"));
            NB_CHECK(nb_buf_shrink(&buf));
            nb_buf_consume(&buf, 15u);
            size_t off = buf.off;
            state->fail_at = state->calls + failure;
            nb_status status = nb_ini_write(ini, &buf);
            state->fail_at = 0;
            if (status == NB_OK) {
                success = true;
                NB_CHECK_STR(nb_buf_cstr(&buf), "keepkey = value\n");
            } else {
                NB_CHECK_INT(status, NB_ERR_NOMEM);
                NB_CHECK_INT(buf.off, off);
                NB_CHECK_STR(nb_buf_cstr(&buf), "keep");
            }
            nb_buf_destroy(&buf);
        }
        NB_CHECK(success);
        nb_ini_free(ini);
    }
}

static uint32_t random_next(uint32_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static void test_random_buffer(void)
{
    uint32_t seed = UINT32_C(0x53b7aa19);
    nb_buf buf;
    nb_buf_init(&buf);
    unsigned char model[4096];
    size_t len = 0;
    for (size_t step = 0; step < 20000u; step++) {
        unsigned operation = random_next(&seed) % 6u;
        size_t count = random_next(&seed) % 33u;
        if (operation == 0u && len + count <= sizeof(model)) {
            unsigned char bytes[32];
            for (size_t i = 0; i < count; i++) {
                bytes[i] = (unsigned char)random_next(&seed);
            }
            NB_CHECK(nb_buf_append(&buf, bytes, count));
            memcpy(model + len, bytes, count);
            len += count;
        } else if (operation == 1u) {
            count = NB_MIN(count, len);
            nb_buf_consume(&buf, count);
            memmove(model, model + count, len - count);
            len -= count;
        } else if (operation == 2u && len != 0 && len + count <= sizeof(model)) {
            count = NB_MIN(count, len);
            size_t start = random_next(&seed) % (len - count + 1u);
            NB_CHECK(nb_buf_append(&buf, nb_buf_cstr(&buf) + start, count));
            memcpy(model + len, model + start, count);
            len += count;
        } else if (operation == 3u) {
            NB_CHECK(nb_buf_shrink(&buf));
        } else if (operation == 4u) {
            nb_buf_compact(&buf);
        } else if (len > 3000u) {
            nb_buf_clear(&buf);
            len = 0;
        }
        NB_CHECK_INT(nb_buf_len(&buf), len);
        NB_CHECK_MEM(nb_buf_cstr(&buf), model, len);
        NB_CHECK_INT((unsigned char)nb_buf_cstr(&buf)[len], 0);
    }
    nb_buf_destroy(&buf);
}

static void test_random_map(void)
{
    uint32_t seed = UINT32_C(0xab341277);
    uintptr_t values[64] = { 0 };
    unsigned order[64];
    size_t count = 0;
    nb_map *map = nb_map_new();
    NB_CHECK(map != NULL);
    if (!map) {
        return;
    }
    char key[128];
    for (size_t step = 0; step < 20000u; step++) {
        unsigned at = random_next(&seed) % 64u;
        size_t key_len = 8u + at;
        memset(key, 'x', key_len);
        key[0] = (char)at;
        nb_str name = nb_str_from_parts(key, key_len);
        if (random_next(&seed) % 3u == 0u) {
            NB_CHECK(nb_map_del(map, name) == (void *)values[at]);
            if (values[at]) {
                size_t index = 0;
                while (index < count && order[index] != at) {
                    index++;
                }
                NB_CHECK(index < count);
                if (index < count) {
                    memmove(order + index, order + index + 1u,
                            (count - index - 1u) * sizeof(order[0]));
                    count--;
                }
                values[at] = 0;
            }
        } else {
            uintptr_t value = (uintptr_t)step + 1u;
            void *old = NULL;
            NB_CHECK(nb_map_put(map, name, (void *)value, &old));
            NB_CHECK(old == (void *)values[at]);
            if (!values[at]) {
                order[count++] = at;
            }
            values[at] = value;
        }
        NB_CHECK(nb_map_get(map, name) == (void *)values[at]);
        NB_CHECK_INT(nb_map_size(map), count);
        if (step % 100u == 0u) {
            nb_map_iter iter;
            nb_map_iter_init(map, &iter);
            for (size_t i = 0; i < count; i++) {
                nb_str actual;
                void *value;
                NB_CHECK(nb_map_iter_next(&iter, &actual, &value));
                NB_CHECK_INT((unsigned char)actual.ptr[0], order[i]);
                NB_CHECK(value == (void *)values[order[i]]);
            }
            NB_CHECK(!nb_map_iter_next(&iter, NULL, NULL));
        }
    }
    nb_map_free(map);
}

static void test_socket_closed_write(void)
{
    nb_socket *listener = NULL;
    nb_socket *client = NULL;
    nb_socket *peer = NULL;
    nb_addr address;
    NB_CHECK_INT(nb_addr_parse("127.0.0.1", 0, &address, NULL), NB_OK);
    nb_status status = nb_socket_open(&listener, NB_AF_INET, NB_SOCK_STREAM, NULL);
    NB_CHECK_INT(status, NB_OK);
    if (status == NB_OK) {
        status = nb_socket_bind(listener, &address, NULL);
        NB_CHECK_INT(status, NB_OK);
    }
    if (status == NB_OK) {
        status = nb_socket_local_addr(listener, &address, NULL);
        NB_CHECK_INT(status, NB_OK);
    }
    if (status == NB_OK) {
        status = nb_socket_listen(listener, 1, NULL);
        NB_CHECK_INT(status, NB_OK);
    }
    if (status == NB_OK) {
        status = nb_socket_open(&client, NB_AF_INET, NB_SOCK_STREAM, NULL);
        NB_CHECK_INT(status, NB_OK);
    }
    if (status == NB_OK) {
        status = nb_socket_connect(client, &address, 1000, NULL);
        NB_CHECK_INT(status, NB_OK);
    }
    if (status == NB_OK) {
        status = nb_socket_accept(listener, &peer, NULL, NULL);
        NB_CHECK_INT(status, NB_OK);
    }
    if (status == NB_OK) {
        NB_CHECK_INT(nb_socket_shutdown(peer, false, true, NULL), NB_OK);
        NB_CHECK_INT(nb_socket_shutdown(client, false, true, NULL), NB_OK);
        size_t sent = 42u;
        NB_CHECK(nb_socket_send(peer, "x", 1u, &sent, NULL) != NB_OK);
        NB_CHECK_INT(sent, 0u);
        NB_CHECK(nb_socket_send(client, "x", 1u, &sent, NULL) != NB_OK);
        NB_CHECK_INT(sent, 0u);
    }
    nb_socket_free(peer);
    nb_socket_free(client);
    nb_socket_free(listener);
}

static void test_path_and_file(checked_allocator *state)
{
    nb_buf buf;
    nb_buf_init(&buf);
    NB_CHECK(nb_buf_append_cstr(&buf, "a/./b/../c"));
    NB_CHECK_INT(nb_path_clean(&buf, nb_buf_cstr(&buf)), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(&buf), "a/c");
    NB_CHECK_INT(nb_path_join(&buf, nb_buf_cstr(&buf), "d"), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(&buf), "a/c/d");
    NB_CHECK_INT(nb_path_join(&buf, "p", nb_buf_cstr(&buf)), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(&buf), "p/a/c/d");
    NB_CHECK_INT(nb_path_absolute(&buf, nb_buf_cstr(&buf), NULL), NB_OK);
    NB_CHECK(nb_buf_len(&buf) >= 7u);
    nb_buf_destroy(&buf);
    nb_buf_init(&buf);
    NB_CHECK_INT(nb_file_temp(&buf, NULL), NB_OK);
    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(&buf), "file contents", 13u, NULL), NB_OK);
    nb_buf path;
    nb_buf_init(&path);
    NB_CHECK(nb_buf_append_str(&path, nb_buf_str(&buf)));
    nb_err err;
    nb_err_clear(&err);
    state->fail_at = state->calls + 1u;
    NB_CHECK_INT(nb_file_read_all(nb_buf_cstr(&path), &buf, 0, &err), NB_ERR_NOMEM);
    state->fail_at = 0;
    NB_CHECK_INT(err.code, NB_ERR_NOMEM);
    NB_CHECK_STR(nb_buf_cstr(&buf), nb_buf_cstr(&path));
    NB_CHECK_INT(nb_file_read_all(nb_buf_cstr(&buf), &buf, 0, NULL), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(&buf), "file contents");
    NB_CHECK_INT(nb_file_read_all(nb_buf_cstr(&path), &buf, 12u, &err), NB_ERR_TOO_BIG);
    NB_CHECK_INT(err.code, NB_ERR_TOO_BIG);
    NB_CHECK_STR(nb_buf_cstr(&buf), "file contents");
    NB_CHECK_INT(nb_file_read_all(nb_buf_cstr(&path), &buf, 13u, NULL), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(&buf), "file contents");
    char growing[65536];
    memset(growing, 'g', sizeof(growing));
    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(&path), growing, sizeof(growing), NULL), NB_OK);
    state->grow_path = nb_buf_cstr(&path);
    state->grew = false;
    NB_CHECK_INT(nb_file_read_all(nb_buf_cstr(&path), &buf, sizeof(growing), &err), NB_ERR_TOO_BIG);
    NB_CHECK(state->grew);
    NB_CHECK_INT(err.code, NB_ERR_TOO_BIG);
    NB_CHECK_STR(nb_buf_cstr(&buf), "file contents");
    state->grow_path = NULL;
    NB_CHECK_INT(nb_file_read_all(nb_buf_cstr(&path), &buf, sizeof(growing) + 1u, NULL), NB_OK);
    NB_CHECK_INT(buf.len, sizeof(growing) + 1u);
    NB_CHECK(buf.ptr[sizeof(growing)] == 'x');
    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(&path), NULL), NB_OK);
    NB_CHECK_INT(nb_file_read_all(nb_buf_cstr(&path), &buf, 0, NULL), NB_ERR_NOT_FOUND);
    NB_CHECK_INT(buf.len, sizeof(growing) + 1u);
    nb_buf_destroy(&path);
    nb_buf_destroy(&buf);
}

static void test_numeric_boundaries(void)
{
    const char embedded[] = { '1', '\0', 'x' };
    double value = 42.0;
    NB_CHECK(!nb_str_to_f64(nb_str_from_parts(embedded, sizeof(embedded)), &value));
    NB_CHECK(value == 42.0);
    int64_t ns = 42;
    NB_CHECK(nb_time_parse_rfc3339("1677-09-21T00:12:43.145224192Z", &ns));
    NB_CHECK_INT(ns, INT64_MIN);
    NB_CHECK(!nb_time_parse_rfc3339("1677-09-21T00:12:43.145224191Z", &ns));
    NB_CHECK_INT(ns, INT64_MIN);
    NB_CHECK(nb_time_parse_rfc3339("2262-04-11T23:47:16.854775807Z", &ns));
    NB_CHECK_INT(ns, INT64_MAX);
    NB_CHECK(!nb_time_parse_rfc3339("2262-04-11T23:47:16.854775808Z", &ns));
    NB_CHECK_INT(ns, INT64_MAX);
}

void test_regressions(void)
{
    checked_allocator state = { 0 };
    nb_allocator alloc = { checked_alloc, checked_realloc, checked_free, &state };
    const nb_allocator *saved = nb_mem_allocator();
    nb_mem_set_allocator(&alloc);
    test_self_copy();
    test_arena_reuse();
    test_encoding_self_input();
    test_failure_atomicity(&state);
    test_ini_transactions(&state);
    test_random_buffer();
    test_random_map();
    test_socket_closed_write();
    test_path_and_file(&state);
    test_numeric_boundaries();
    NB_CHECK_INT(state.live, 0u);
    nb_mem_set_allocator(saved);
}
