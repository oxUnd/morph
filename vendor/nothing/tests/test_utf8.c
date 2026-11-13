#include "test.h"

#include "nb/nbase.h"

#include <string.h>

static nb_str bytes_str(const unsigned char *bytes, size_t len)
{
    return nb_str_from_parts((const char *)bytes, len);
}

static void test_utf8_validate(void)
{
    static const unsigned char valid[] = {
        'A', 0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87, 0xF0, 0x9F, 0x98, 0x80
    };
    size_t offset = 0;
    NB_CHECK(nb_utf8_valid(bytes_str(valid, sizeof(valid))));
    NB_CHECK_INT(nb_utf8_count(bytes_str(valid, sizeof(valid))), 4);
    NB_CHECK_INT(nb_utf8_validate(bytes_str(valid, sizeof(valid)), &offset),
                 NB_UTF8_OK);
    NB_CHECK_INT(offset, sizeof(valid));

    static const unsigned char invalid_cont[] = { 0x80 };
    static const unsigned char truncated2[] = { 0xE4, 0xB8 };
    static const unsigned char truncated4[] = { 0xF0, 0x9F, 0x98 };
    static const unsigned char overlong2[] = { 0xC0, 0x80 };
    static const unsigned char overlong_lead[] = { 0xC1, 0xBF };
    static const unsigned char overlong3[] = { 0xE0, 0x80, 0x80 };
    static const unsigned char overlong4[] = { 0xF0, 0x80, 0x80, 0x80 };
    static const unsigned char surrogate[] = { 0xED, 0xA0, 0x80 };
    static const unsigned char too_large4[] = { 0xF4, 0x90, 0x80, 0x80 };
    static const unsigned char too_large_lead[] = { 0xF5, 0x80, 0x80, 0x80 };
    static const unsigned char bad_cont[] = { 0xE4, 0x28, 0xAD };

    static const struct {
        const unsigned char *bytes;
        size_t len;
        nb_utf8_status status;
    } cases[] = {
        { invalid_cont, sizeof(invalid_cont), NB_UTF8_INVALID },
        { truncated2, sizeof(truncated2), NB_UTF8_TRUNCATED },
        { truncated4, sizeof(truncated4), NB_UTF8_TRUNCATED },
        { overlong2, sizeof(overlong2), NB_UTF8_OVERLONG },
        { overlong_lead, sizeof(overlong_lead), NB_UTF8_OVERLONG },
        { overlong3, sizeof(overlong3), NB_UTF8_OVERLONG },
        { overlong4, sizeof(overlong4), NB_UTF8_OVERLONG },
        { surrogate, sizeof(surrogate), NB_UTF8_SURROGATE },
        { too_large4, sizeof(too_large4), NB_UTF8_TOO_LARGE },
        { too_large_lead, sizeof(too_large_lead), NB_UTF8_TOO_LARGE },
        { bad_cont, sizeof(bad_cont), NB_UTF8_INVALID },
    };

    for (size_t i = 0; i < NB_ARRAY_SIZE(cases); i++) {
        nb_str s = bytes_str(cases[i].bytes, cases[i].len);
        offset = 99;
        NB_CHECK_INT(nb_utf8_validate(s, &offset), cases[i].status);
        NB_CHECK_INT(offset, 0);
        NB_CHECK(!nb_utf8_valid(s));
        NB_CHECK_INT(nb_utf8_count(s), SIZE_MAX);
    }

    static const unsigned char prefixed_bad[] = { 'A', 0xE4, 0xB8 };
    offset = 99;
    NB_CHECK_INT(nb_utf8_validate(bytes_str(prefixed_bad,
                                            sizeof(prefixed_bad)), &offset),
                 NB_UTF8_TRUNCATED);
    NB_CHECK_INT(offset, 1);
    NB_CHECK(nb_utf8_valid(NB_STR_NULL));
    NB_CHECK_INT(nb_utf8_count(NB_STR_NULL), 0);
}

static void check_utf8_scan(nb_str text)
{
    size_t expected_offset = 0;
    nb_utf8_status expected = NB_UTF8_OK;
    while (expected_offset < text.len) {
        uint32_t cp;
        size_t next = expected_offset;
        if (!nb_utf8_decode(text, &next, &cp, &expected)) {
            break;
        }
        expected_offset = next;
    }
    size_t offset = SIZE_MAX;
    NB_CHECK_INT(nb_utf8_validate(text, &offset), expected);
    NB_CHECK_INT(offset, expected_offset);
    NB_CHECK_INT(nb_utf8_valid(text), expected == NB_UTF8_OK);
}

static void test_utf8_scan_boundaries(void)
{
    /* Exact allocations expose a block load past the view to ASan. */
    for (size_t alignment = 0; alignment < 16u; alignment++) {
        for (size_t len = 0; len <= 96u; len++) {
            size_t size = alignment + len;
            unsigned char *storage = malloc(size ? size : 1u);
            NB_CHECK(storage != NULL);
            if (!storage) {
                return;
            }
            unsigned char *data = storage + alignment;
            for (size_t i = 0; i < len; i++) {
                data[i] = (unsigned char)((i * 31u) & 0x7Fu);
            }
            check_utf8_scan(bytes_str(data, len));
            for (size_t pos = 0; pos < len; pos++) {
                unsigned char saved = data[pos];
                data[pos] = 0xFFu;
                check_utf8_scan(bytes_str(data, len));
                data[pos] = saved;
            }
            free(storage);
        }
    }

    static const struct {
        unsigned char bytes[4];
        size_t len;
    } sequences[] = {
        { { 0xC2, 0x80 }, 2 }, { { 0xE4, 0xB8, 0xAD }, 3 },
        { { 0xF4, 0x8F, 0xBF, 0xBF }, 4 }, { { 0x80 }, 1 },
        { { 0xC0, 0x80 }, 2 }, { { 0xE0, 0x80, 0x80 }, 3 },
        { { 0xED, 0xA0, 0x80 }, 3 }, { { 0xF4, 0x90, 0x80, 0x80 }, 4 },
        { { 0xE4, 'x', 0xAD }, 3 }, { { 0xFF }, 1 }
    };
    unsigned char data[160];
    for (size_t alignment = 0; alignment < 16u; alignment++) {
        for (size_t prefix = 0; prefix <= 64u; prefix++) {
            for (size_t seq = 0; seq < NB_ARRAY_SIZE(sequences); seq++) {
                memset(data, 'a', sizeof(data));
                memcpy(data + alignment + prefix, sequences[seq].bytes,
                       sequences[seq].len);
                check_utf8_scan(bytes_str(data + alignment, 128u));
                for (size_t tail = 1; tail <= sequences[seq].len; tail++) {
                    check_utf8_scan(bytes_str(data + alignment, prefix + tail));
                }
            }
        }
    }
    size_t offset = SIZE_MAX;
    NB_CHECK_INT(nb_utf8_validate((nb_str){ NULL, 5u }, &offset), NB_UTF8_INVALID);
    NB_CHECK_INT(offset, 0);
}

static void test_utf8_decode_encode(void)
{
    static const unsigned char mixed[] = {
        'A', 0xE4, 0xB8, 0xAD, 0xF0, 0x9F, 0x98, 0x80
    };
    nb_str s = bytes_str(mixed, sizeof(mixed));
    size_t index = 0;
    uint32_t cp = 0;
    nb_utf8_status status = NB_UTF8_INVALID;

    NB_CHECK(nb_utf8_decode(s, &index, &cp, &status));
    NB_CHECK_INT(status, NB_UTF8_OK);
    NB_CHECK_INT(cp, 'A');
    NB_CHECK_INT(index, 1);

    NB_CHECK(nb_utf8_decode(s, &index, &cp, &status));
    NB_CHECK_INT(cp, 0x4E2D);
    NB_CHECK_INT(index, 4);

    NB_CHECK(nb_utf8_decode(s, &index, &cp, &status));
    NB_CHECK_INT(cp, 0x1F600);
    NB_CHECK_INT(index, 8);

    size_t before = index;
    NB_CHECK(!nb_utf8_decode(s, &index, &cp, &status));
    NB_CHECK_INT(status, NB_UTF8_TRUNCATED);
    NB_CHECK_INT(index, before);

    static const struct {
        uint32_t cp;
        size_t len;
        unsigned char first;
        unsigned char last;
    } encoded[] = {
        { 0x0000, 1, 0x00, 0x00 },
        { 0x007F, 1, 0x7F, 0x7F },
        { 0x0080, 2, 0xC2, 0x80 },
        { 0x07FF, 2, 0xDF, 0xBF },
        { 0x0800, 3, 0xE0, 0x80 },
        { 0x4E2D, 3, 0xE4, 0xAD },
        { 0xFFFF, 3, 0xEF, 0xBF },
        { 0x10000, 4, 0xF0, 0x80 },
        { 0x10FFFF, 4, 0xF4, 0xBF },
    };

    for (size_t i = 0; i < NB_ARRAY_SIZE(encoded); i++) {
        char buf[4];
        size_t len = nb_utf8_encode(encoded[i].cp, buf);
        NB_CHECK_INT(len, encoded[i].len);
        NB_CHECK_INT((unsigned char)buf[0], encoded[i].first);
        NB_CHECK_INT((unsigned char)buf[len - 1u], encoded[i].last);

        nb_str encoded_str = nb_str_from_parts(buf, len);
        size_t next = 0;
        uint32_t decoded = 0;
        NB_CHECK(nb_utf8_decode(encoded_str, &next, &decoded, &status));
        NB_CHECK_INT(decoded, encoded[i].cp);
        NB_CHECK_INT(next, len);
    }

    char buf[4];
    NB_CHECK_INT(nb_utf8_encode(0xD800, buf), 0);
    NB_CHECK_INT(nb_utf8_encode(0xDFFF, buf), 0);
    NB_CHECK_INT(nb_utf8_encode(0x110000, buf), 0);
    NB_CHECK_INT(nb_utf8_encode(0xFFFFFFFFu, buf), 0);
}

static void test_utf8_offset_slice(void)
{
    static const unsigned char text[] = {
        0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87, 'a', 'b'
    };
    nb_str s = bytes_str(text, sizeof(text));

    NB_CHECK_INT(nb_utf8_count(s), 4);
    NB_CHECK_INT(nb_utf8_offset(s, 0), 0);
    NB_CHECK_INT(nb_utf8_offset(s, 1), 3);
    NB_CHECK_INT(nb_utf8_offset(s, 2), 6);
    NB_CHECK_INT(nb_utf8_offset(s, 3), 7);
    NB_CHECK_INT(nb_utf8_offset(s, 4), 8);
    NB_CHECK_INT(nb_utf8_offset(s, 5), SIZE_MAX);

    nb_str slice = nb_utf8_slice(s, 1, 2);
    NB_CHECK_INT(slice.len, 4);
    if (slice.ptr) {
        NB_CHECK_MEM(slice.ptr, text + 3, 4);
    }

    nb_str empty = nb_utf8_slice(s, 4, 0);
    NB_CHECK_INT(empty.len, 0);
    NB_CHECK_INT((size_t)(empty.ptr - (const char *)text), 8);

    nb_str past = nb_utf8_slice(s, 5, 1);
    NB_CHECK(past.ptr == NULL && past.len == 0);

    nb_str whole = nb_utf8_slice(s, 0, SIZE_MAX);
    NB_CHECK_INT(whole.len, sizeof(text));

    nb_str cut = bytes_str(text, 4);
    NB_CHECK(nb_utf8_slice(cut, 0, 2).ptr == NULL);

    static const unsigned char ascii_bad_tail[] = { 'a', 0xFF };
    nb_str bad_tail = bytes_str(ascii_bad_tail, sizeof(ascii_bad_tail));
    NB_CHECK_INT(nb_utf8_offset(bad_tail, 0), SIZE_MAX);
    NB_CHECK_INT(nb_utf8_offset(bad_tail, 1), SIZE_MAX);
    NB_CHECK_INT(nb_utf8_offset(bad_tail, 2), SIZE_MAX);
    NB_CHECK(nb_utf8_slice(bad_tail, 0, 0).ptr == NULL);
    NB_CHECK(nb_utf8_slice(bad_tail, 0, 1).ptr == NULL);
    NB_CHECK(nb_utf8_slice(bad_tail, 1, 1).ptr == NULL);

    static const unsigned char utf8_bad_tail[] = { 0xC3, 0xA9, 0xFF };
    nb_str bad_utf8_tail =
        bytes_str(utf8_bad_tail, sizeof(utf8_bad_tail));
    NB_CHECK_INT(nb_utf8_offset(bad_utf8_tail, 0), SIZE_MAX);
    NB_CHECK_INT(nb_utf8_offset(bad_utf8_tail, 1), SIZE_MAX);
    NB_CHECK_INT(nb_utf8_offset(bad_utf8_tail, 2), SIZE_MAX);
    NB_CHECK(nb_utf8_slice(bad_utf8_tail, 0, 1).ptr == NULL);

    static const unsigned char bad_middle[] = { 'a', 0xFF, 'b' };
    nb_str bad_in_middle = bytes_str(bad_middle, sizeof(bad_middle));
    NB_CHECK_INT(nb_utf8_offset(bad_in_middle, 0), SIZE_MAX);
    NB_CHECK_INT(nb_utf8_offset(bad_in_middle, 1), SIZE_MAX);
    NB_CHECK(nb_utf8_slice(bad_in_middle, 0, 1).ptr == NULL);

    nb_str empty_input = NB_STR_NULL;
    NB_CHECK_INT(nb_utf8_offset(empty_input, 0), 0);
    NB_CHECK_INT(nb_utf8_offset(empty_input, 1), SIZE_MAX);
}

static void test_utf8_trim(void)
{
    static const unsigned char spaced[] = {
        ' ', 0xE3, 0x80, 0x80, 0xE4, 0xB8, 0xAD,
        0xC2, 0xA0, 'x', 0xE3, 0x80, 0x80
    };
    nb_str s = bytes_str(spaced, sizeof(spaced));
    nb_str trimmed = nb_utf8_trim(s);
    NB_CHECK_INT(trimmed.len, 6);
    NB_CHECK_INT(nb_utf8_count(trimmed), 3);
    if (trimmed.ptr) {
        NB_CHECK_MEM(trimmed.ptr, spaced + 4, 6);
    }

    nb_str ascii = nb_utf8_trim(NB_STR_LIT(" \t\nabc \r\n"));
    NB_CHECK_INT(ascii.len, 3);
    NB_CHECK_MEM(ascii.ptr, "abc", 3);

    static const unsigned char invalid[] = { 0xE4, 0xB8 };
    nb_str untouched = nb_utf8_trim(bytes_str(invalid, sizeof(invalid)));
    NB_CHECK_INT(untouched.len, 2);
    NB_CHECK_MEM(untouched.ptr, invalid, 2);

    NB_CHECK(nb_utf8_is_space(0x3000));
    NB_CHECK(nb_utf8_is_space(0x00A0));
    NB_CHECK(nb_utf8_is_space('\n'));
    NB_CHECK(!nb_utf8_is_space('A'));
    NB_CHECK(!nb_utf8_is_space(0x200B));
}

static void test_utf8_bom(void)
{
    static const unsigned char with_bom[] = {
        0xEF, 0xBB, 0xBF, 'a', 'b', 'c'
    };
    nb_str s = nb_utf8_strip_bom(bytes_str(with_bom, sizeof(with_bom)));
    NB_CHECK_INT(s.len, 3);
    NB_CHECK_MEM(s.ptr, "abc", 3);

    static const unsigned char partial[] = { 0xEF, 0xBB, 'x' };
    s = nb_utf8_strip_bom(bytes_str(partial, sizeof(partial)));
    NB_CHECK_INT(s.len, 3);

    s = nb_utf8_strip_bom(NB_STR_LIT("abc"));
    NB_CHECK_INT(s.len, 3);
}

static void test_utf8_ini_bom(void)
{
    static const unsigned char ini[] = {
        0xEF, 0xBB, 0xBF, 'n', 'a', 'm', 'e', ' ', '=', ' ',
        'n', 'b', 'a', 's', 'e', '\n'
    };
    nb_err err;
    nb_err_clear(&err);
    nb_ini *doc = nb_ini_from_string(bytes_str(ini, sizeof(ini)), &err);
    NB_CHECK(doc != NULL);
    if (doc) {
        NB_CHECK_STR(nb_ini_get(doc, NULL, "name"), "nbase");
        nb_ini_free(doc);
    }
}

typedef struct utf8_log_capture {
    char buf[4096];
    size_t len;
} utf8_log_capture;

static void utf8_capture_sink(void *ctx, nb_log_level level,
                              const char *record, size_t len)
{
    (void)level;
    utf8_log_capture *capture = ctx;
    size_t n = len < sizeof(capture->buf) - 1u ? len : sizeof(capture->buf) - 1u;
    if (n != 0) {
        memcpy(capture->buf, record, n);
    }
    capture->buf[n] = '\0';
    capture->len = n;
}

static void test_utf8_log_full_record(void)
{
    static char pattern[2200];
    memset(pattern, 'x', 2046);
    pattern[2046] = (char)0xE4;
    pattern[2047] = (char)0xB8;
    pattern[2048] = (char)0xAD;
    pattern[2049] = '\0';

    utf8_log_capture capture = { { 0 }, 0 };
    nb_log *log = nb_log_new(NB_LOG_INFO, utf8_capture_sink, &capture);
    NB_CHECK(log != NULL);
    if (!log) {
        return;
    }
    nb_log_write(log, NB_LOG_ERROR, "%s", pattern);
    NB_CHECK(capture.len != 0 && capture.buf[capture.len - 1u] == '\n');
    NB_CHECK(strstr(capture.buf, pattern) != NULL);
    NB_CHECK(capture.len > strlen(pattern));
    NB_CHECK(nb_utf8_valid(nb_str_from_parts(capture.buf, capture.len)));
    nb_log_free(log);
}

static void test_unicode_properties_and_casefold(void)
{
    NB_CHECK_INT(nb_utf8_width_cp(0x0897), 0);
    NB_CHECK_INT(nb_utf8_width_cp(0x1fae9), 2);
    NB_CHECK_INT(nb_utf8_width_cp(0x1faea), 2);
    NB_CHECK_INT(nb_utf8_classify_codepoint(0x01c5), NB_UTF8_CLASS_UPPER);
    NB_CHECK_INT(nb_utf8_classify_codepoint(0x0301), NB_UTF8_CLASS_MARK);
    NB_CHECK_INT(nb_utf8_classify_codepoint(0x202f), NB_UTF8_CLASS_SEPARATOR);
    NB_CHECK_INT(nb_utf8_classify_codepoint(0x110000), NB_UTF8_CLASS_OTHER);

    nb_buf folded = {0};
    NB_CHECK_INT(nb_utf8_casefold(NB_STR_LIT("Straße E\xcc\x81"), &folded, NULL), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(&folded), "strasse é");
    nb_buf_clear(&folded);
    NB_CHECK_INT(nb_utf8_casefold(NB_STR_LIT(""), &folded, NULL), NB_OK);
    NB_CHECK_INT(nb_buf_len(&folded), 0);
    NB_CHECK_INT(nb_utf8_casefold(nb_str_from_parts("\xff", 1), &folded, NULL), NB_ERR_INVAL);
    NB_CHECK_INT(nb_buf_len(&folded), 0);
    nb_buf_destroy(&folded);

    const char partial[] = {'A', (char)0xe4, (char)0xb8};
    NB_CHECK_INT(nb_utf8_clamp_bytes(partial, sizeof(partial)), 1);
    unsigned cp = 0;
    size_t count = 77;
    NB_CHECK(!nb_utf8_read(partial + 1, 2, &cp, &count));
    NB_CHECK_INT(count, 77);
}

void test_utf8(void)
{
    test_unicode_properties_and_casefold();
    test_utf8_validate();
    test_utf8_scan_boundaries();
    test_utf8_decode_encode();
    test_utf8_offset_slice();
    test_utf8_trim();
    test_utf8_bom();
    test_utf8_ini_bom();
    test_utf8_log_full_record();
}
