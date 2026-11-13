#include "test.h"

#include "nb/buf.h"
#include "nb/encoding.h"
#include "nb/file.h"
#include "nb/ini.h"
#include "nb/str.h"

static void check_base64(const char *text, nb_b64_variant variant,
                         bool expected)
{
    nb_str input = nb_str_from_cstr(text);
    NB_CHECK_INT(nb_base64_valid(input, variant), expected ? 1 : 0);

    nb_buf *out = nb_buf_new();
    NB_CHECK(out != NULL);
    if (!out) {
        return;
    }
    nb_buf_append_cstr(out, "keep");
    nb_status status = nb_base64_decode(input, variant, out, NULL);
    if (expected) {
        NB_CHECK_INT(status, NB_OK);
    } else {
        NB_CHECK_INT(status, NB_ERR_PARSE);
        NB_CHECK_STR(nb_buf_cstr(out), "keep");
    }
    nb_buf_free(out);
}

static void test_base64_valid(void)
{
    NB_CHECK(nb_base64_valid(NB_STR_NULL, NB_B64_STD));
    NB_CHECK(nb_base64_valid(NB_STR_LIT(""), NB_B64_STD));
    NB_CHECK(nb_base64_valid(NB_STR_LIT("    "), NB_B64_URL));

    check_base64("", NB_B64_STD, true);
    check_base64("   ", NB_B64_URL, true);
    check_base64("aGVsbG8=", NB_B64_STD, true);
    check_base64("aGVsbG8", NB_B64_STD, true);
    check_base64("AA==", NB_B64_STD, true);
    check_base64("AAA=", NB_B64_STD, true);
    check_base64("AA", NB_B64_STD, true);
    check_base64("AAA", NB_B64_STD, true);
    check_base64("AAAA", NB_B64_STD, true);
    check_base64("aGVs\nbG8=", NB_B64_STD, true);
    check_base64("aGVs bG8=", NB_B64_STD, true);
    check_base64("AA== \t\r\n", NB_B64_STD, true);
    check_base64("a+b/", NB_B64_STD, true);
    check_base64("----", NB_B64_URL, true);
    check_base64("____", NB_B64_URL, true);

    check_base64("A", NB_B64_STD, false);
    check_base64("AB", NB_B64_STD, false);
    check_base64("AAB", NB_B64_STD, false);
    check_base64("AB==", NB_B64_STD, false);
    check_base64("AAB=", NB_B64_STD, false);
    check_base64("AAA==", NB_B64_STD, false);
    check_base64("AA=A", NB_B64_STD, false);
    check_base64("A===", NB_B64_STD, false);
    check_base64("====", NB_B64_STD, false);
    check_base64("AA===", NB_B64_STD, false);
    check_base64("AA==\nAA", NB_B64_STD, false);
    check_base64("AA*=", NB_B64_STD, false);
    check_base64("a+b/", NB_B64_URL, false);
    check_base64("----", NB_B64_STD, false);
    check_base64("____", NB_B64_STD, false);

    nb_str missing = { NULL, 3u };
    NB_CHECK(!nb_base64_valid(missing, NB_B64_STD));
    NB_CHECK(!nb_base64_valid(NB_STR_LIT("AAAA"), (nb_b64_variant)42));

    nb_buf *out = nb_buf_new();
    NB_CHECK_INT(nb_base64_decode(NB_STR_LIT("AAAA"), (nb_b64_variant)42,
                                  out, NULL),
                 NB_ERR_INVAL);
    NB_CHECK_INT(nb_base64_decode(NB_STR_LIT("aGVs\nbG8="), NB_B64_STD,
                                  out, NULL),
                 NB_OK);
    NB_CHECK_STR(nb_buf_cstr(out), "hello");
    nb_buf_free(out);
}

static int reference_sextet(unsigned char c, nb_b64_variant variant)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == (variant == NB_B64_STD ? '+' : '-')) {
        return 62;
    }
    return c == (variant == NB_B64_STD ? '/' : '_') ? 63 : -1;
}

/* Scalar oracle for alphabet, whitespace, padding, and unused tail bits. */
static bool reference_base64(nb_str input, nb_b64_variant variant,
                              unsigned char *decoded, size_t *len)
{
    unsigned char compact[64];
    size_t size = 0;
    for (size_t i = 0; i < input.len; i++) {
        unsigned char c = (unsigned char)input.ptr[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            compact[size++] = c;
        }
    }
    size_t chars = size;
    while (chars != 0 && compact[chars - 1u] == '=') {
        chars--;
    }
    size_t padding = size - chars;
    if (padding > 2u || (padding != 0 && size % 4u != 0) || chars % 4u == 1u) {
        return false;
    }
    uint32_t accumulator = 0;
    unsigned bits = 0;
    *len = 0;
    for (size_t i = 0; i < chars; i++) {
        int value = reference_sextet(compact[i], variant);
        if (value < 0) {
            return false;
        }
        accumulator = (accumulator << 6) | (unsigned)value;
        bits += 6u;
        if (bits >= 8u) {
            bits -= 8u;
            decoded[(*len)++] = (unsigned char)(accumulator >> bits);
        }
    }
    return (accumulator & ((1u << bits) - 1u)) == 0;
}

static void test_decode_byte_values(void)
{
    nb_buf out;
    nb_buf_init(&out);
    unsigned char expected[64];
    char input[40];
    for (unsigned variant = 0; variant < 2u; variant++) {
        for (size_t pos = 0; pos < sizeof(input); pos++) {
            for (unsigned byte = 0; byte < 256u; byte++) {
                memset(input, 'A', sizeof(input));
                input[pos] = (char)byte;
                nb_str text = nb_str_from_parts(input, sizeof(input));
                size_t len = 0;
                bool valid = reference_base64(text, (nb_b64_variant)variant,
                                               expected, &len);
                NB_CHECK_INT(nb_base64_valid(text, (nb_b64_variant)variant), valid);
                nb_buf_clear(&out);
                NB_CHECK(nb_buf_append_cstr(&out, "discarded-prefix/keep"));
                nb_buf_consume(&out, 17u);
                size_t old_len = out.len;
                size_t old_off = out.off;
                nb_status status = nb_base64_decode(text, (nb_b64_variant)variant,
                                                     &out, NULL);
                NB_CHECK_INT(status, valid ? NB_OK : NB_ERR_PARSE);
                if (valid) {
                    NB_CHECK_INT(nb_buf_len(&out), 4u + len);
                    NB_CHECK_MEM(nb_buf_cstr(&out), "keep", 4u);
                    NB_CHECK_MEM(nb_buf_cstr(&out) + 4u, expected, len);
                } else {
                    NB_CHECK_INT(out.len, old_len);
                    NB_CHECK_INT(out.off, old_off);
                    NB_CHECK_STR(out.ptr, "discarded-prefix/keep");
                }
                NB_CHECK_INT(out.ptr[out.len], '\0');
            }
        }
    }
    static const char digits[] = "0123456789abcdefABCDEF";
    for (size_t pos = 0; pos < sizeof(input); pos++) {
        for (unsigned byte = 0; byte < 256u; byte++) {
            memset(input, '0', sizeof(input));
            input[pos] = (char)byte;
            bool valid = byte != 0 && strchr(digits, (int)byte) != NULL;
            nb_buf_clear(&out);
            NB_CHECK(nb_buf_append_cstr(&out, "discarded-prefix/keep"));
            nb_buf_consume(&out, 17u);
            size_t old_len = out.len;
            size_t old_off = out.off;
            nb_status status = nb_hex_decode(nb_str_from_parts(input, sizeof(input)),
                                              &out, NULL);
            NB_CHECK_INT(status, valid ? NB_OK : NB_ERR_PARSE);
            if (valid) {
                unsigned value = byte <= '9' ? byte - '0' :
                    byte <= 'F' ? byte - 'A' + 10u : byte - 'a' + 10u;
                memset(expected, 0, sizeof(input) / 2u);
                expected[pos / 2u] = (unsigned char)(pos % 2u ? value : value << 4);
                NB_CHECK_INT(nb_buf_len(&out), 4u + sizeof(input) / 2u);
                NB_CHECK_MEM(nb_buf_cstr(&out), "keep", 4u);
                NB_CHECK_MEM(nb_buf_cstr(&out) + 4u, expected, sizeof(input) / 2u);
            } else {
                NB_CHECK_INT(out.len, old_len);
                NB_CHECK_INT(out.off, old_off);
                NB_CHECK_STR(out.ptr, "discarded-prefix/keep");
            }
            NB_CHECK_INT(out.ptr[out.len], '\0');
        }
    }
    nb_buf_destroy(&out);
}

static void test_ini_quotes(void)
{
    const char *text =
        "name = nbase\n"
        "[t]\n"
        "dq = \"a b\"\n"
        "sq = 'a b'\n"
        "esc = \"a\\\\b\\\"c\\'d\\ne\\rf\\tg\"\n"
        "single = 'x\\'y'\n"
        "spaces = \"  padded  \"\n"
        "url = http://x/#frag\n"
        "hash = a#b\n"
        "semi = a;b\n"
        "directhash=#notcomment\n"
        "directsemi=;semi\n"
        "spacedhash = #comment\n"
        "trim =   a b   \n"
        "note = a ; comment\n"
        "note2 = a\t# comment\n"
        "qcomment = \"v\" ; note\n"
        "qhash = \"a # b\"\n"
        "empty = \"\"\n"
        "dup = first\n"
        "dup = second\n";
    nb_err err;
    nb_err_clear(&err);
    nb_ini *ini = nb_ini_from_string(nb_str_from_cstr(text), &err);
    NB_CHECK(ini != NULL);
    if (!ini) {
        return;
    }

    NB_CHECK_STR(nb_ini_get(ini, "", "name"), "nbase");
    NB_CHECK_STR(nb_ini_get(ini, "t", "dq"), "a b");
    NB_CHECK_STR(nb_ini_get(ini, "t", "sq"), "a b");
    NB_CHECK_STR(nb_ini_get(ini, "t", "esc"), "a\\b\"c'd\ne\rf\tg");
    NB_CHECK_STR(nb_ini_get(ini, "t", "single"), "x'y");
    NB_CHECK_STR(nb_ini_get(ini, "t", "spaces"), "  padded  ");
    NB_CHECK_STR(nb_ini_get(ini, "t", "url"), "http://x/#frag");
    NB_CHECK_STR(nb_ini_get(ini, "t", "hash"), "a#b");
    NB_CHECK_STR(nb_ini_get(ini, "t", "semi"), "a;b");
    NB_CHECK_STR(nb_ini_get(ini, "t", "directhash"), "#notcomment");
    NB_CHECK_STR(nb_ini_get(ini, "t", "directsemi"), ";semi");
    NB_CHECK_STR(nb_ini_get(ini, "t", "spacedhash"), "");
    NB_CHECK_STR(nb_ini_get(ini, "t", "trim"), "a b");
    NB_CHECK_STR(nb_ini_get(ini, "t", "note"), "a");
    NB_CHECK_STR(nb_ini_get(ini, "t", "note2"), "a");
    NB_CHECK_STR(nb_ini_get(ini, "t", "qcomment"), "v");
    NB_CHECK_STR(nb_ini_get(ini, "t", "qhash"), "a # b");
    NB_CHECK_STR(nb_ini_get(ini, "t", "empty"), "");
    NB_CHECK_STR(nb_ini_get(ini, "t", "dup"), "second");
    NB_CHECK_INT(nb_ini_count(ini, "t"), 18);
    NB_CHECK_INT(nb_ini_section_count(ini), 2);

    nb_ini_free(ini);
}

static void test_ini_invalid(void)
{
    static const char *const bad[] = {
        "k = \"unterminated\n",
        "k = 'unterminated\n",
        "k = \"a\" junk\n",
        "k = \"a\";note\n",
        "k = \"a\\\n",
        "[broken\n",
        "key\n",
    };

    for (size_t i = 0; i < NB_ARRAY_SIZE(bad); i++) {
        nb_err err;
        nb_err_clear(&err);
        nb_ini *ini = nb_ini_from_string(nb_str_from_cstr(bad[i]), &err);
        NB_CHECK(ini == NULL);
        NB_CHECK_INT(err.code, NB_ERR_PARSE);
        NB_CHECK(strstr(err.msg, "line 1") != NULL);
    }

    nb_err err;
    nb_err_clear(&err);
    nb_ini *ini = nb_ini_from_string(NB_STR_LIT("ok = 1\nbad = \"x\n"), &err);
    NB_CHECK(ini == NULL);
    NB_CHECK_INT(err.code, NB_ERR_PARSE);
    NB_CHECK(strstr(err.msg, "line 2") != NULL);
}

static void test_ini_atomic(void)
{
    nb_err err;
    nb_err_clear(&err);
    nb_ini *ini = nb_ini_from_string(NB_STR_LIT("existing = keep\n"), &err);
    NB_CHECK(ini != NULL);
    if (!ini) {
        return;
    }

    NB_CHECK_INT(nb_ini_parse(ini,
                              NB_STR_LIT("added = yes\nbroken = \"oops\n"),
                              &err),
                 NB_ERR_PARSE);
    NB_CHECK(!nb_ini_has(ini, "", "added"));
    NB_CHECK_STR(nb_ini_get(ini, "", "existing"), "keep");
    NB_CHECK_INT(nb_ini_count(ini, ""), 1);

    NB_CHECK_INT(nb_ini_parse(ini, NB_STR_LIT("added = yes\n"), &err), NB_OK);
    NB_CHECK_STR(nb_ini_get(ini, "", "added"), "yes");
    NB_CHECK_INT(nb_ini_count(ini, ""), 2);
    nb_ini_free(ini);
}

static void test_ini_set_validation(void)
{
    static const struct {
        const char *section;
        const char *key;
        const char *value;
    } bad[] = {
        { "", "a=b", "v" },
        { "", "a\nb", "v" },
        { "", "a\rb", "v" },
        { "", " a", "v" },
        { "", "a ", "v" },
        { "", "", "v" },
        { "", "[a", "v" },
        { "", ";a", "v" },
        { "", "#a", "v" },
        { "", "\xEF\xBB\xBFk", "v" },
        { "", "\xEF\xBB\xBF", "v" },
        { "a]", "k", "v" },
        { "a\nb", "k", "v" },
        { "a\rb", "k", "v" },
        { " a", "k", "v" },
        { "a ", "k", "v" },
    };
    nb_ini *ini = nb_ini_new();
    NB_CHECK(ini != NULL);
    if (!ini) {
        return;
    }
    NB_CHECK_INT(nb_ini_set(ini, "", "sentinel", "keep"), NB_OK);

    for (size_t i = 0; i < NB_ARRAY_SIZE(bad); i++) {
        NB_CHECK_INT(nb_ini_set(ini, bad[i].section, bad[i].key,
                                bad[i].value),
                     NB_ERR_INVAL);
        NB_CHECK_INT(nb_ini_count(ini, ""), 1);
        NB_CHECK_INT(nb_ini_section_count(ini), 1);
        NB_CHECK_STR(nb_ini_get(ini, "", "sentinel"), "keep");
    }

    nb_buf *out = nb_buf_new();
    NB_CHECK(out != NULL);
    if (out) {
        NB_CHECK_INT(nb_ini_write(ini, out), NB_OK);
        NB_CHECK_STR(nb_buf_cstr(out), "sentinel = keep\n");
        nb_buf_free(out);
    }

    /* Valid edge cases must not be over-rejected: NULL selects the global
     * section and interior whitespace is allowed in names. */
    NB_CHECK_INT(nb_ini_set(ini, NULL, "a b", "1"), NB_OK);
    NB_CHECK_INT(nb_ini_set(ini, "s p", "a b", "2"), NB_OK);
    NB_CHECK_STR(nb_ini_get(ini, "", "a b"), "1");
    NB_CHECK_STR(nb_ini_get(ini, "s p", "a b"), "2");
    nb_ini_free(ini);
}

static void test_ini_parse_rejects_nul(void)
{
    static const char nul_text[] = "added = yes\nbad = a\0b\n";
    nb_str text = nb_str_from_parts(nul_text, sizeof(nul_text) - 1u);
    nb_err err;
    nb_err_clear(&err);
    nb_ini *ini = nb_ini_from_string(NB_STR_LIT("existing = keep\n"), &err);
    NB_CHECK(ini != NULL);
    if (!ini) {
        return;
    }

    NB_CHECK_INT(nb_ini_parse(ini, text, &err), NB_ERR_PARSE);
    NB_CHECK_INT(err.code, NB_ERR_PARSE);
    NB_CHECK(!nb_ini_has(ini, "", "added"));
    NB_CHECK(!nb_ini_has(ini, "", "bad"));
    NB_CHECK_STR(nb_ini_get(ini, "", "existing"), "keep");
    NB_CHECK_INT(nb_ini_count(ini, ""), 1);

    nb_err_clear(&err);
    nb_ini *rejected = nb_ini_from_string(text, &err);
    NB_CHECK(rejected == NULL);
    NB_CHECK_INT(err.code, NB_ERR_PARSE);
    nb_ini_free(ini);
}

static void test_ini_section_comments(void)
{
    static const char *const bad[] = {
        "[a];comment\nk = v\n",
        "[a]#comment\nk = v\n",
    };
    for (size_t i = 0; i < NB_ARRAY_SIZE(bad); i++) {
        nb_err err;
        nb_err_clear(&err);
        nb_ini *ini = nb_ini_from_string(nb_str_from_cstr(bad[i]), &err);
        NB_CHECK(ini == NULL);
        NB_CHECK_INT(err.code, NB_ERR_PARSE);
    }

    static const char *const good[] = {
        "[a] ;comment\nk = v\n",
        "[a]\t#comment\nk = v\n",
        "[a]\nk = v\n",
    };
    for (size_t i = 0; i < NB_ARRAY_SIZE(good); i++) {
        nb_err err;
        nb_err_clear(&err);
        nb_ini *ini = nb_ini_from_string(nb_str_from_cstr(good[i]), &err);
        NB_CHECK(ini != NULL);
        if (ini) {
            NB_CHECK_STR(nb_ini_get(ini, "a", "k"), "v");
            NB_CHECK_INT(nb_ini_section_count(ini), 1);
            nb_ini_free(ini);
        }
    }

    nb_err err;
    nb_err_clear(&err);
    nb_ini *bare = nb_ini_from_string(NB_STR_LIT("[a]\n"), &err);
    NB_CHECK(bare != NULL);
    if (bare) {
        nb_ini_free(bare);
    }

    nb_err_clear(&err);
    nb_ini *empty = nb_ini_from_string(NB_STR_LIT("=v\n"), &err);
    NB_CHECK(empty == NULL);
    NB_CHECK_INT(err.code, NB_ERR_PARSE);

    nb_err_clear(&err);
    empty = nb_ini_from_string(NB_STR_LIT("  = v\n"), &err);
    NB_CHECK(empty == NULL);
    NB_CHECK_INT(err.code, NB_ERR_PARSE);
}

static void test_ini_global_section_order(void)
{
    nb_err err;
    nb_err_clear(&err);
    nb_ini *ini = nb_ini_from_string(NB_STR_LIT("[a]\nx = 1\n"), &err);
    NB_CHECK(ini != NULL);
    if (!ini) {
        return;
    }

    /* The global entry is inserted after section a; it must still be written
     * before the section header. */
    NB_CHECK_INT(nb_ini_set(ini, "", "g", "2"), NB_OK);
    NB_CHECK_STR(nb_ini_get(ini, "", "g"), "2");

    nb_buf *out = nb_buf_new();
    NB_CHECK(out != NULL);
    if (!out) {
        nb_ini_free(ini);
        return;
    }
    NB_CHECK_INT(nb_ini_write(ini, out), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(out), "g = 2\n[a]\nx = 1\n");

    nb_ini *copy = nb_ini_from_string(nb_buf_str(out), &err);
    NB_CHECK(copy != NULL);
    if (copy) {
        NB_CHECK_STR(nb_ini_get(copy, "", "g"), "2");
        NB_CHECK_STR(nb_ini_get(copy, "a", "x"), "1");
        NB_CHECK(!nb_ini_has(copy, "a", "g"));
        NB_CHECK(!nb_ini_has(copy, "", "x"));
        nb_ini_free(copy);
    }
    nb_buf_free(out);
    nb_ini_free(ini);
}

static void test_ini_roundtrip(void)
{
    static const char *const values[] = {
        "",
        "plain",
        "a b",
        " leading",
        "trailing ",
        "  ",
        "a#b",
        "a;b",
        "a\"b",
        "a'b",
        "a\\b",
        "line\nbreak",
        "tab\there",
        "cr\rhere",
        "\001control",
        "value = with equals",
        "[bracket]",
    };
    nb_err err;
    nb_err_clear(&err);
    nb_ini *ini = nb_ini_new();
    NB_CHECK(ini != NULL);
    if (!ini) {
        return;
    }

    char keys[NB_ARRAY_SIZE(values)][8];
    for (size_t i = 0; i < NB_ARRAY_SIZE(values); i++) {
        snprintf(keys[i], sizeof(keys[i]), "k%zu", i);
        NB_CHECK_INT(nb_ini_set(ini, "", keys[i], values[i]), NB_OK);
        NB_CHECK_INT(nb_ini_set(ini, "s", keys[i], values[i]), NB_OK);
    }

    nb_buf *out = nb_buf_new();
    NB_CHECK(out != NULL);
    if (!out) {
        nb_ini_free(ini);
        return;
    }
    NB_CHECK_INT(nb_ini_write(ini, out), NB_OK);
    nb_ini *copy = nb_ini_from_string(nb_buf_str(out), &err);
    NB_CHECK(copy != NULL);
    if (copy) {
        for (size_t i = 0; i < NB_ARRAY_SIZE(values); i++) {
            NB_CHECK_STR(nb_ini_get(copy, "", keys[i]), values[i]);
            NB_CHECK_STR(nb_ini_get(copy, "s", keys[i]), values[i]);
        }
        nb_ini_free(copy);
    }
    nb_buf_free(out);
    nb_ini_free(ini);

    nb_ini *plain = nb_ini_new();
    NB_CHECK(plain != NULL);
    if (plain) {
        nb_buf *text = nb_buf_new();
        NB_CHECK_INT(nb_ini_set(plain, "", "plain", "hello"), NB_OK);
        NB_CHECK_INT(nb_ini_write(plain, text), NB_OK);
        NB_CHECK_STR(nb_buf_cstr(text), "plain = hello\n");

        nb_buf_clear(text);
        NB_CHECK_INT(nb_ini_set(plain, "", "quoted", " a "), NB_OK);
        NB_CHECK_INT(nb_ini_write(plain, text), NB_OK);
        NB_CHECK(strstr(nb_buf_cstr(text), "quoted = \" a \"") != NULL);
        nb_buf_free(text);
        nb_ini_free(plain);
    }
}

static void test_ini_save(void)
{
    nb_err err;
    nb_err_clear(&err);
    NB_CHECK_INT(nb_ini_save(NULL, "unused.ini", &err), NB_ERR_INVAL);

    nb_ini *ini = nb_ini_new();
    NB_CHECK(ini != NULL);
    if (!ini) {
        return;
    }
    NB_CHECK_INT(nb_ini_save(ini, NULL, &err), NB_ERR_INVAL);
    NB_CHECK_INT(nb_ini_set(ini, "", "value", "  quoted  "), NB_OK);
    NB_CHECK_INT(nb_ini_set(ini, "s", "hash", "a#b"), NB_OK);

    nb_buf *path = nb_buf_new();
    NB_CHECK(path != NULL);
    if (!path) {
        nb_ini_free(ini);
        return;
    }
    nb_status status = nb_file_temp(path, &err);
    NB_CHECK_INT(status, NB_OK);
    if (status == NB_OK) {
        status = nb_ini_save(ini, nb_buf_cstr(path), &err);
        NB_CHECK_INT(status, NB_OK);
        if (status == NB_OK) {
            nb_ini *loaded = nb_ini_load(nb_buf_cstr(path), &err);
            NB_CHECK(loaded != NULL);
            if (loaded) {
                NB_CHECK_STR(nb_ini_get(loaded, "", "value"), "  quoted  ");
                NB_CHECK_STR(nb_ini_get(loaded, "s", "hash"), "a#b");
                nb_ini_free(loaded);
            }
        }
        nb_file_remove(nb_buf_cstr(path), &err);
    }
    nb_buf_free(path);
    nb_ini_free(ini);
}

void test_config(void)
{
    test_base64_valid();
    test_decode_byte_values();
    test_ini_quotes();
    test_ini_invalid();
    test_ini_atomic();
    test_ini_set_validation();
    test_ini_parse_rejects_nul();
    test_ini_section_comments();
    test_ini_global_section_order();
    test_ini_roundtrip();
    test_ini_save();
}
