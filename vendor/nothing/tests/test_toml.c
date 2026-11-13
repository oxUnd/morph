#include "test.h"

#include "nb/buf.h"
#include "nb/toml.h"

#include <locale.h>
#include <math.h>
#include <string.h>

/*
 * TOML 1.0 semantic tests. Every valid document is inspected through the
 * public AST API; a non-NULL parse result alone never counts as a pass.
 */

/* ---------- parse helpers ---------- */

static nb_toml *parse_ok_n(const char *src, size_t len)
{
    nb_err err;
    nb_err_clear(&err);
    nb_toml *doc = nb_toml_parse(nb_str_from_parts(src, len), &err);
    if (!doc) {
        fprintf(stderr, "FAIL %s:%d: valid document rejected: %s\n", __FILE__,
                __LINE__, err.msg);
        nb_test_failures++;
    }
    return doc;
}

static nb_toml *parse_ok(const char *src)
{
    return parse_ok_n(src, strlen(src));
}

static void expect_parse_error_n(const char *src, size_t len)
{
    nb_err err;
    nb_err_clear(&err);
    nb_toml *doc = nb_toml_parse(nb_str_from_parts(src, len), &err);
    if (doc) {
        fprintf(stderr, "FAIL %s:%d: invalid document accepted: %.*s\n",
                __FILE__, __LINE__, (int)len, src);
        nb_test_failures++;
        nb_toml_free(doc);
        return;
    }
    if (err.code != NB_ERR_PARSE) {
        fprintf(stderr, "FAIL %s:%d: expected NB_ERR_PARSE, got %d (%s)\n",
                __FILE__, __LINE__, (int)err.code, err.msg);
        nb_test_failures++;
    }
}

static void expect_parse_error(const char *src)
{
    expect_parse_error_n(src, strlen(src));
}

/* ---------- value helpers ---------- */

static const nb_toml_value *doc_root(const nb_toml *doc)
{
    const nb_toml_value *root = nb_toml_root(doc);
    NB_CHECK(root != NULL);
    return root;
}

static const nb_toml_value *table_get(const nb_toml_value *table,
                                      const char *key)
{
    if (!table) {
        return NULL;
    }
    return nb_toml_table_get_cstr(table, key);
}

static const nb_toml_value *doc_get(const nb_toml *doc, const char *key)
{
    return table_get(doc_root(doc), key);
}

static const nb_toml_value *array_at(const nb_toml_value *array, size_t index)
{
    if (!array) {
        return NULL;
    }
    return nb_toml_array_get(array, index);
}

static void check_type(const nb_toml_value *value, nb_toml_type expected)
{
    if (!value) {
        NB_CHECK(false);
        return;
    }
    NB_CHECK_INT(nb_toml_type_of(value), expected);
}

static void check_table_size(const nb_toml_value *table, size_t expected)
{
    if (!table) {
        NB_CHECK(false);
        return;
    }
    NB_CHECK_INT(nb_toml_table_size(table), expected);
}

static void check_array_size(const nb_toml_value *array, size_t expected)
{
    if (!array) {
        NB_CHECK(false);
        return;
    }
    NB_CHECK_INT(nb_toml_array_size(array), expected);
}

static void check_int_value(const nb_toml_value *value, int64_t expected)
{
    int64_t actual = 0;
    bool ok = nb_toml_get_integer(value, &actual);
    NB_CHECK(ok);
    if (ok) {
        NB_CHECK_INT(actual, expected);
    }
}

static void check_float_value(const nb_toml_value *value, double expected)
{
    double actual = 0.0;
    bool ok = nb_toml_get_float(value, &actual);
    NB_CHECK(ok);
    if (!ok) {
        return;
    }
    double tolerance = 1e-12 * fabs(expected);
    if (tolerance < 1e-300) {
        tolerance = 1e-300;
    }
    NB_CHECK(fabs(actual - expected) <= tolerance);
}

static void check_bool_value(const nb_toml_value *value, bool expected)
{
    bool actual = !expected;
    bool ok = nb_toml_get_bool(value, &actual);
    NB_CHECK(ok);
    if (ok) {
        NB_CHECK(actual == expected);
    }
}

static void check_string_bytes(const nb_toml_value *value, const char *expected,
                               size_t len)
{
    nb_str actual = { NULL, 0 };
    bool ok = nb_toml_get_string(value, &actual);
    NB_CHECK(ok);
    if (!ok) {
        return;
    }
    NB_CHECK_INT(actual.len, len);
    if (actual.len == len && len > 0) {
        NB_CHECK(actual.ptr != NULL);
        NB_CHECK_MEM(actual.ptr, expected, len);
    }
}

static void check_datetime_value(const nb_toml_value *value,
                                 nb_toml_datetime_kind kind, int year,
                                 int month, int day, int hour, int minute,
                                 int second, uint32_t nanosecond,
                                 int offset_minutes)
{
    nb_toml_datetime dt;
    memset(&dt, 0, sizeof(dt));
    dt.year = -1;
    bool ok = nb_toml_get_datetime(value, &dt);
    NB_CHECK(ok);
    if (!ok) {
        return;
    }
    NB_CHECK_INT(dt.kind, kind);
    NB_CHECK_INT(dt.year, year);
    NB_CHECK_INT(dt.month, month);
    NB_CHECK_INT(dt.day, day);
    NB_CHECK_INT(dt.hour, hour);
    NB_CHECK_INT(dt.minute, minute);
    NB_CHECK_INT(dt.second, second);
    NB_CHECK_INT(dt.nanosecond, nanosecond);
    NB_CHECK_INT(dt.offset_minutes, offset_minutes);
}

static void check_table_order(const nb_toml_value *table,
                              const char *const *keys, size_t count)
{
    if (!table) {
        NB_CHECK(false);
        return;
    }
    nb_toml_table_iter iter;
    nb_toml_table_iter_init(table, &iter);
    nb_str key;
    const nb_toml_value *value = NULL;
    size_t i = 0;
    while (nb_toml_table_iter_next(&iter, &key, &value)) {
        NB_CHECK(value != NULL);
        if (i < count) {
            NB_CHECK(nb_str_eq_cstr(key, keys[i]));
        } else {
            NB_CHECK(false);
        }
        i++;
    }
    NB_CHECK_INT(i, count);
}

#define CHECK_STRING(value, expected)                                       \
    do {                                                                    \
        const char *nb_expected = (expected);                               \
        size_t nb_expected_len = strlen(nb_expected);                       \
        nb_str nb_actual = { NULL, 0 };                                     \
        bool nb_ok = nb_toml_get_string((value), &nb_actual);               \
        NB_CHECK(nb_ok);                                                    \
        if (nb_ok) {                                                        \
            NB_CHECK_INT(nb_actual.len, nb_expected_len);                   \
            if (nb_actual.len == nb_expected_len && nb_expected_len > 0) {  \
                NB_CHECK(nb_actual.ptr != NULL);                            \
                if (nb_actual.ptr) {                                        \
                    NB_CHECK_MEM(nb_actual.ptr, nb_expected,                \
                                 nb_expected_len);                          \
                }                                                           \
            }                                                               \
        }                                                                   \
    } while (0)

/* ---------- strings ---------- */

static void test_toml_strings(void)
{
    static const char src[] =
        "basic = \"hello\"\n"
        "empty = \"\"\n"
        "esc = \"\\b\\t\\n\\f\\r\\\"\\\\\"\n"
        "esc_unicode = \"\\u00E9\"\n"
        "esc_unicode_wide = \"\\U0001F600\"\n"
        "nul = \"a\\u0000b\"\n"
        "lit = 'C:\\Users\\nodejs\\templates'\n"
        "lit_quote = 'say \"hi\"'\n"
        "lit_empty = ''\n"
        "utf8 = \"\xE4\xB8\xAD\xE6\x96\x87\"\n"
        "raw_tab = \"a\tb\"\n"
        "ml_basic = \"\"\"\nline1\nline2\"\"\"\n"
        "ml_basic_indent = \"\"\"\n  indented\"\"\"\n"
        "ml_basic_cont = \"\"\"abc\\\n   def\"\"\"\n"
        "ml_basic_quotes = \"\"\"a\"\"b\"\"\"\n"
        "ml_lit = '''\nline1\nline2'''\n"
        "ml_lit_quotes = '''a''b'''\n";

    nb_toml *doc = parse_ok(src);
    if (!doc) {
        return;
    }

    CHECK_STRING(doc_get(doc, "basic"), "hello");
    CHECK_STRING(doc_get(doc, "empty"), "");
    CHECK_STRING(doc_get(doc, "esc"), "\b\t\n\f\r\"\\");
    CHECK_STRING(doc_get(doc, "lit"), "C:\\Users\\nodejs\\templates");
    CHECK_STRING(doc_get(doc, "lit_quote"), "say \"hi\"");
    CHECK_STRING(doc_get(doc, "lit_empty"), "");
    CHECK_STRING(doc_get(doc, "raw_tab"), "a\tb");
    CHECK_STRING(doc_get(doc, "ml_basic"), "line1\nline2");
    CHECK_STRING(doc_get(doc, "ml_basic_indent"), "  indented");
    CHECK_STRING(doc_get(doc, "ml_basic_cont"), "abcdef");
    CHECK_STRING(doc_get(doc, "ml_basic_quotes"), "a\"\"b");
    CHECK_STRING(doc_get(doc, "ml_lit"), "line1\nline2");
    CHECK_STRING(doc_get(doc, "ml_lit_quotes"), "a''b");

    /* Copy one value to prove the AST does not require a trailing NUL. */
    {
        nb_str text = { NULL, 0 };
        char copied[16];
        bool ok = nb_toml_get_string(doc_get(doc, "basic"), &text);
        NB_CHECK(ok);
        if (ok) {
            NB_CHECK(text.len < sizeof(copied));
            if (text.len < sizeof(copied)) {
                memcpy(copied, text.ptr, text.len);
                copied[text.len] = '\0';
                NB_CHECK_STR(copied, "hello");
            }
        }
    }

    static const char e_acute[] = { (char)0xC3, (char)0xA9 };
    check_string_bytes(doc_get(doc, "esc_unicode"), e_acute,
                       sizeof(e_acute));

    static const char emoji[] = { (char)0xF0, (char)0x9F, (char)0x98,
                                  (char)0x80 };
    check_string_bytes(doc_get(doc, "esc_unicode_wide"), emoji,
                       sizeof(emoji));

    static const char utf8_bytes[] = { (char)0xE4, (char)0xB8, (char)0xAD,
                                       (char)0xE6, (char)0x96, (char)0x87 };
    check_string_bytes(doc_get(doc, "utf8"), utf8_bytes,
                       sizeof(utf8_bytes));

    static const char nul_bytes[] = { 'a', '\0', 'b' };
    check_string_bytes(doc_get(doc, "nul"), nul_bytes, sizeof(nul_bytes));

    check_type(doc_get(doc, "basic"), NB_TOML_STRING);
    check_type(doc_get(doc, "nul"), NB_TOML_STRING);
    nb_toml_free(doc);
}

/* ---------- integers ---------- */

static void test_toml_integers(void)
{
    static const char src[] =
        "dec = 42\n"
        "neg = -17\n"
        "plus = +99\n"
        "zero = 0\n"
        "neg_zero = -0\n"
        "hex = 0xDEADBEEF\n"
        "hex_under = 0xdead_beef\n"
        "hex_zero = 0x0000FF\n"
        "oct = 0o755\n"
        "bin = 0b1101\n"
        "under = 1_000_000\n"
        "max = 9223372036854775807\n"
        "min = -9223372036854775808\n";

    nb_toml *doc = parse_ok(src);
    if (!doc) {
        return;
    }

    check_int_value(doc_get(doc, "dec"), 42);
    check_int_value(doc_get(doc, "neg"), -17);
    check_int_value(doc_get(doc, "plus"), 99);
    check_int_value(doc_get(doc, "zero"), 0);
    check_int_value(doc_get(doc, "neg_zero"), 0);
    check_int_value(doc_get(doc, "hex"), 3735928559LL);
    check_int_value(doc_get(doc, "hex_under"), 3735928559LL);
    check_int_value(doc_get(doc, "hex_zero"), 255);
    check_int_value(doc_get(doc, "oct"), 493);
    check_int_value(doc_get(doc, "bin"), 13);
    check_int_value(doc_get(doc, "under"), 1000000);
    check_int_value(doc_get(doc, "max"), INT64_MAX);
    check_int_value(doc_get(doc, "min"), INT64_MIN);

    check_type(doc_get(doc, "dec"), NB_TOML_INTEGER);
    check_type(doc_get(doc, "hex"), NB_TOML_INTEGER);
    nb_toml_free(doc);
}

/* ---------- floats ---------- */

static void test_toml_floats(void)
{
    static const char src[] =
        "pi = 3.1415\n"
        "neg = -0.01\n"
        "exp = 5e+22\n"
        "exp2 = -2E-2\n"
        "exp3 = 1e0\n"
        "exp_zero = 1e06\n"
        "exp_under = 1e1_0\n"
        "under = 224_617.445_991_228\n"
        "inf = inf\n"
        "pinf = +inf\n"
        "ninf = -inf\n"
        "nan = nan\n"
        "pnan = +nan\n"
        "nnan = -nan\n"
        "nzero = -0.0\n"
        "zero = 0.0\n";

    nb_toml *doc = parse_ok(src);
    if (!doc) {
        return;
    }

    check_float_value(doc_get(doc, "pi"), 3.1415);
    check_float_value(doc_get(doc, "neg"), -0.01);
    check_float_value(doc_get(doc, "exp"), 5e+22);
    check_float_value(doc_get(doc, "exp2"), -2e-2);
    check_float_value(doc_get(doc, "exp3"), 1.0);
    check_float_value(doc_get(doc, "exp_zero"), 1e6);
    check_float_value(doc_get(doc, "exp_under"), 1e10);
    check_float_value(doc_get(doc, "under"), 224617.445991228);
    check_float_value(doc_get(doc, "zero"), 0.0);

    {
        double f = 0.0;
        bool ok = nb_toml_get_float(doc_get(doc, "inf"), &f);
        NB_CHECK(ok);
        if (ok) {
            NB_CHECK(isinf(f) && f > 0.0);
        }
    }
    {
        double f = 0.0;
        bool ok = nb_toml_get_float(doc_get(doc, "pinf"), &f);
        NB_CHECK(ok);
        if (ok) {
            NB_CHECK(isinf(f) && f > 0.0);
        }
    }
    {
        double f = 0.0;
        bool ok = nb_toml_get_float(doc_get(doc, "ninf"), &f);
        NB_CHECK(ok);
        if (ok) {
            NB_CHECK(isinf(f) && f < 0.0);
        }
    }
    {
        double f = 0.0;
        bool ok = nb_toml_get_float(doc_get(doc, "nan"), &f);
        NB_CHECK(ok);
        if (ok) {
            NB_CHECK(isnan(f));
        }
    }
    {
        double f = 0.0;
        bool ok = nb_toml_get_float(doc_get(doc, "pnan"), &f);
        NB_CHECK(ok);
        if (ok) {
            NB_CHECK(isnan(f));
        }
    }
    {
        double f = 0.0;
        bool ok = nb_toml_get_float(doc_get(doc, "nnan"), &f);
        NB_CHECK(ok);
        if (ok) {
            NB_CHECK(isnan(f));
        }
    }
    {
        double f = 1.0;
        bool ok = nb_toml_get_float(doc_get(doc, "nzero"), &f);
        NB_CHECK(ok);
        if (ok) {
            NB_CHECK(signbit(f) != 0);
        }
    }

    check_type(doc_get(doc, "pi"), NB_TOML_FLOAT);
    check_type(doc_get(doc, "inf"), NB_TOML_FLOAT);
    nb_toml_free(doc);
}

/* ---------- booleans ---------- */

static void test_toml_booleans(void)
{
    nb_toml *doc = parse_ok("t = true\nf = false\n");
    if (!doc) {
        return;
    }
    check_bool_value(doc_get(doc, "t"), true);
    check_bool_value(doc_get(doc, "f"), false);
    check_type(doc_get(doc, "t"), NB_TOML_BOOLEAN);
    nb_toml_free(doc);
}

/* ---------- datetimes ---------- */

static void test_toml_datetimes(void)
{
    static const char src[] =
        "ld = 1979-05-27\n"
        "leap = 2000-02-29\n"
        "lt = 07:32:00\n"
        "lt_frac = 00:32:00.999999\n"
        "lt_frac9 = 00:32:00.123456789\n"
        "ldt = 1979-05-27T07:32:00\n"
        "ldt_space = 1979-05-27 07:32:00\n"
        "odt_z = 1979-05-27T00:32:00Z\n"
        "odt_z_frac = 1979-05-27T00:32:00.999999Z\n"
        "odt_neg = 1979-05-27T00:32:00.999999-07:00\n"
        "odt_pos = 1979-05-27T00:32:00+05:30\n"
        "leap_z = 1990-12-31T23:59:60Z\n"
        "leap_lt = 23:59:60\n";

    nb_toml *doc = parse_ok(src);
    if (!doc) {
        return;
    }

    check_datetime_value(doc_get(doc, "ld"), NB_TOML_LOCAL_DATE, 1979, 5, 27,
                         0, 0, 0, 0, 0);
    check_datetime_value(doc_get(doc, "leap"), NB_TOML_LOCAL_DATE, 2000, 2,
                         29, 0, 0, 0, 0, 0);
    check_datetime_value(doc_get(doc, "lt"), NB_TOML_LOCAL_TIME, 0, 0, 0, 7,
                         32, 0, 0, 0);
    check_datetime_value(doc_get(doc, "lt_frac"), NB_TOML_LOCAL_TIME, 0, 0, 0,
                         0, 32, 0, 999999000, 0);
    check_datetime_value(doc_get(doc, "lt_frac9"), NB_TOML_LOCAL_TIME, 0, 0, 0,
                         0, 32, 0, 123456789, 0);
    check_datetime_value(doc_get(doc, "ldt"), NB_TOML_LOCAL_DATETIME, 1979, 5,
                         27, 7, 32, 0, 0, 0);
    check_datetime_value(doc_get(doc, "ldt_space"), NB_TOML_LOCAL_DATETIME,
                         1979, 5, 27, 7, 32, 0, 0, 0);
    check_datetime_value(doc_get(doc, "odt_z"), NB_TOML_OFFSET_DATETIME, 1979,
                         5, 27, 0, 32, 0, 0, 0);
    check_datetime_value(doc_get(doc, "odt_z_frac"),
                         NB_TOML_OFFSET_DATETIME, 1979, 5, 27, 0, 32, 0,
                         999999000, 0);
    check_datetime_value(doc_get(doc, "odt_neg"), NB_TOML_OFFSET_DATETIME, 1979,
                         5, 27, 0, 32, 0, 999999000, -420);
    check_datetime_value(doc_get(doc, "odt_pos"), NB_TOML_OFFSET_DATETIME, 1979,
                         5, 27, 0, 32, 0, 0, 330);
    check_datetime_value(doc_get(doc, "leap_z"), NB_TOML_OFFSET_DATETIME, 1990,
                         12, 31, 23, 59, 60, 0, 0);
    check_datetime_value(doc_get(doc, "leap_lt"), NB_TOML_LOCAL_TIME, 0, 0, 0,
                         23, 59, 60, 0, 0);

    check_type(doc_get(doc, "ld"), NB_TOML_DATETIME);
    check_type(doc_get(doc, "odt_neg"), NB_TOML_DATETIME);
    nb_toml_free(doc);
}

/* ---------- tables, headers and dotted keys ---------- */

static void test_toml_tables(void)
{
    static const char src[] =
        "title = \"root\"\n"
        "[owner]\n"
        "name = \"Tom\"\n"
        "dob = 1979-05-27T07:32:00Z\n"
        "[owner.info]\n"
        "\"\" = \"blank\"\n"
        "\"quoted key\" = 1\n"
        "[database]\n"
        "ports = [ 8000, 8001, 8002 ]\n"
        "enabled = true\n"
        "[servers.alpha]\n"
        "ip = \"10.0.0.1\"\n"
        "[servers.beta]\n"
        "ip = \"10.0.0.2\"\n";

    nb_toml *doc = parse_ok(src);
    if (!doc) {
        return;
    }

    const nb_toml_value *root = doc_root(doc);
    check_table_size(root, 4);
    NB_CHECK(table_get(root, "missing") == NULL);

    const nb_toml_value *owner = doc_get(doc, "owner");
    check_type(owner, NB_TOML_TABLE);
    check_table_size(owner, 3);
    CHECK_STRING(table_get(owner, "name"), "Tom");
    check_datetime_value(table_get(owner, "dob"), NB_TOML_OFFSET_DATETIME, 1979,
                         5, 27, 7, 32, 0, 0, 0);

    const nb_toml_value *info = table_get(owner, "info");
    check_type(info, NB_TOML_TABLE);
    check_table_size(info, 2);
    nb_str empty_key = nb_str_from_parts("", 0);
    CHECK_STRING(nb_toml_table_get(info, empty_key), "blank");
    check_int_value(table_get(info, "quoted key"), 1);

    const nb_toml_value *database = doc_get(doc, "database");
    check_table_size(database, 2);
    const nb_toml_value *ports = table_get(database, "ports");
    check_type(ports, NB_TOML_ARRAY);
    check_array_size(ports, 3);
    check_int_value(array_at(ports, 0), 8000);
    check_int_value(array_at(ports, 1), 8001);
    check_int_value(array_at(ports, 2), 8002);
    check_bool_value(table_get(database, "enabled"), true);

    const nb_toml_value *servers = doc_get(doc, "servers");
    check_type(servers, NB_TOML_TABLE);
    check_table_size(servers, 2);
    CHECK_STRING(table_get(table_get(servers, "alpha"), "ip"), "10.0.0.1");
    CHECK_STRING(table_get(table_get(servers, "beta"), "ip"), "10.0.0.2");

    nb_toml_free(doc);
}

static void test_toml_quoted_and_dotted_keys(void)
{
    static const char dotted[] =
        "fruit.apple.color = \"red\"\n"
        "fruit.apple.taste.sweet = true\n"
        "fruit.orange = 2\n"
        "site.\"google.com\" = true\n"
        "\"a.b\" = 3\n"
        "\"\" = \"root-empty\"\n"
        "a.\"\" = 1\n"
        "\"esc\\u0041key\" = 5\n";

    nb_toml *doc = parse_ok(dotted);
    if (!doc) {
        return;
    }

    const nb_toml_value *root = doc_root(doc);
    check_table_size(root, 6);

    const nb_toml_value *fruit = doc_get(doc, "fruit");
    check_table_size(fruit, 2);
    const nb_toml_value *apple = table_get(fruit, "apple");
    check_table_size(apple, 2);
    CHECK_STRING(table_get(apple, "color"), "red");
    check_bool_value(table_get(table_get(apple, "taste"), "sweet"), true);
    check_int_value(table_get(fruit, "orange"), 2);

    const nb_toml_value *site = doc_get(doc, "site");
    check_bool_value(table_get(site, "google.com"), true);
    check_int_value(doc_get(doc, "a.b"), 3);
    CHECK_STRING(nb_toml_table_get(root, nb_str_from_parts("", 0)),
                 "root-empty");
    check_int_value(table_get(doc_get(doc, "a"),
                              ""),
                    1);
    check_int_value(doc_get(doc, "escAkey"), 5);

    nb_toml_free(doc);

    static const char quoted_header[] =
        "[dog.\"tater.man\"]\n"
        "type.name = \"pug\"\n";
    doc = parse_ok(quoted_header);
    if (!doc) {
        return;
    }
    const nb_toml_value *dog = doc_get(doc, "dog");
    const nb_toml_value *tater = table_get(dog, "tater.man");
    CHECK_STRING(table_get(table_get(tater, "type"), "name"), "pug");
    nb_toml_free(doc);

    static const char empty_header[] = "[\"\"]\nx = 1\n";
    doc = parse_ok(empty_header);
    if (!doc) {
        return;
    }
    const nb_toml_value *empty_table =
        nb_toml_table_get(doc_root(doc), nb_str_from_parts("", 0));
    check_type(empty_table, NB_TOML_TABLE);
    check_int_value(table_get(empty_table, "x"), 1);
    nb_toml_free(doc);
}

/* ---------- arrays ---------- */

static void test_toml_arrays(void)
{
    static const char src[] =
        "ints = [1, 2, 3]\n"
        "nested = [[1, 2], [3]]\n"
        "mixed = [1, \"two\", 3.0, true]\n"
        "multi = [\n"
        "  1,\n"
        "  2, # comment\n"
        "  3,\n"
        "]\n"
        "empty = []\n"
        "tables = [{ x = 1 }, { x = 2 }]\n";

    nb_toml *doc = parse_ok(src);
    if (!doc) {
        return;
    }

    const nb_toml_value *ints = doc_get(doc, "ints");
    check_type(ints, NB_TOML_ARRAY);
    check_array_size(ints, 3);
    check_int_value(array_at(ints, 0), 1);
    check_int_value(array_at(ints, 1), 2);
    check_int_value(array_at(ints, 2), 3);

    const nb_toml_value *nested = doc_get(doc, "nested");
    check_array_size(nested, 2);
    const nb_toml_value *nested0 = array_at(nested, 0);
    const nb_toml_value *nested1 = array_at(nested, 1);
    check_type(nested0, NB_TOML_ARRAY);
    check_array_size(nested0, 2);
    check_int_value(array_at(nested0, 0), 1);
    check_int_value(array_at(nested0, 1), 2);
    check_array_size(nested1, 1);
    check_int_value(array_at(nested1, 0), 3);

    const nb_toml_value *mixed = doc_get(doc, "mixed");
    check_array_size(mixed, 4);
    check_int_value(array_at(mixed, 0), 1);
    CHECK_STRING(array_at(mixed, 1), "two");
    check_float_value(array_at(mixed, 2), 3.0);
    check_bool_value(array_at(mixed, 3), true);

    const nb_toml_value *multi = doc_get(doc, "multi");
    check_array_size(multi, 3);
    check_int_value(array_at(multi, 0), 1);
    check_int_value(array_at(multi, 1), 2);
    check_int_value(array_at(multi, 2), 3);

    check_array_size(doc_get(doc, "empty"), 0);

    const nb_toml_value *tables = doc_get(doc, "tables");
    check_array_size(tables, 2);
    check_int_value(table_get(array_at(tables, 0), "x"), 1);
    check_int_value(table_get(array_at(tables, 1), "x"), 2);

    nb_toml_free(doc);
}

static void test_toml_inline_tables(void)
{
    static const char src[] =
        "point = { x = 1, y = 2 }\n"
        "nested = { a = { b = \"c\" }, d = [1, 2] }\n"
        "dotted = { a.b = 1 }\n"
        "empty = {}\n";

    nb_toml *doc = parse_ok(src);
    if (!doc) {
        return;
    }

    const nb_toml_value *point = doc_get(doc, "point");
    check_type(point, NB_TOML_TABLE);
    check_table_size(point, 2);
    check_int_value(table_get(point, "x"), 1);
    check_int_value(table_get(point, "y"), 2);

    const nb_toml_value *nested = doc_get(doc, "nested");
    check_table_size(nested, 2);
    CHECK_STRING(table_get(table_get(nested, "a"), "b"), "c");
    const nb_toml_value *d = table_get(nested, "d");
    check_array_size(d, 2);
    check_int_value(array_at(d, 0), 1);
    check_int_value(array_at(d, 1), 2);

    check_int_value(table_get(table_get(doc_get(doc, "dotted"), "a"), "b"),
                    1);
    check_table_size(doc_get(doc, "empty"), 0);

    nb_toml_free(doc);
}

/* ---------- arrays of tables ---------- */

static void test_toml_arrays_of_tables(void)
{
    static const char src[] =
        "[[products]]\n"
        "name = \"Hammer\"\n"
        "sku = 738594937\n"
        "\n"
        "[[products]]\n"
        "\n"
        "[[products]]\n"
        "name = \"Nail\"\n"
        "sku = 284758393\n"
        "color = \"gray\"\n";

    nb_toml *doc = parse_ok(src);
    if (!doc) {
        return;
    }

    const nb_toml_value *products = doc_get(doc, "products");
    check_type(products, NB_TOML_ARRAY);
    check_array_size(products, 3);

    const nb_toml_value *first = array_at(products, 0);
    check_table_size(first, 2);
    CHECK_STRING(table_get(first, "name"), "Hammer");
    check_int_value(table_get(first, "sku"), 738594937);

    const nb_toml_value *second = array_at(products, 1);
    check_type(second, NB_TOML_TABLE);
    check_table_size(second, 0);

    const nb_toml_value *third = array_at(products, 2);
    check_table_size(third, 3);
    CHECK_STRING(table_get(third, "name"), "Nail");
    check_int_value(table_get(third, "sku"), 284758393);
    CHECK_STRING(table_get(third, "color"), "gray");

    nb_toml_free(doc);

    static const char nested_src[] =
        "[[fruits]]\n"
        "name = \"apple\"\n"
        "[fruits.physical]\n"
        "color = \"red\"\n"
        "[[fruits.varieties]]\n"
        "name = \"red delicious\"\n"
        "[[fruits.varieties]]\n"
        "name = \"granny smith\"\n"
        "[[fruits]]\n"
        "name = \"banana\"\n"
        "[[fruits.varieties]]\n"
        "name = \"plantain\"\n";

    doc = parse_ok(nested_src);
    if (!doc) {
        return;
    }

    const nb_toml_value *fruits = doc_get(doc, "fruits");
    check_array_size(fruits, 2);

    const nb_toml_value *apple = array_at(fruits, 0);
    check_table_size(apple, 3);
    CHECK_STRING(table_get(apple, "name"), "apple");
    CHECK_STRING(table_get(table_get(apple, "physical"), "color"), "red");
    const nb_toml_value *varieties = table_get(apple, "varieties");
    check_array_size(varieties, 2);
    CHECK_STRING(table_get(array_at(varieties, 0), "name"),
                 "red delicious");
    CHECK_STRING(table_get(array_at(varieties, 1), "name"),
                 "granny smith");

    const nb_toml_value *banana = array_at(fruits, 1);
    check_table_size(banana, 2);
    CHECK_STRING(table_get(banana, "name"), "banana");
    const nb_toml_value *banana_varieties = table_get(banana, "varieties");
    check_array_size(banana_varieties, 1);
    CHECK_STRING(table_get(array_at(banana_varieties, 0), "name"),
                 "plantain");

    nb_toml_free(doc);
}

/* ---------- iteration order ---------- */

static void test_toml_iteration_order(void)
{
    static const char src[] =
        "z = 1\n"
        "a = 2\n"
        "m = 3\n"
        "[t]\n"
        "c = 4\n"
        "b = 5\n";

    nb_toml *doc = parse_ok(src);
    if (!doc) {
        return;
    }

    static const char *const root_order[] = { "z", "a", "m", "t" };
    check_table_order(doc_root(doc), root_order, NB_ARRAY_SIZE(root_order));

    static const char *const table_order[] = { "c", "b" };
    check_table_order(doc_get(doc, "t"), table_order,
                      NB_ARRAY_SIZE(table_order));

    nb_toml_free(doc);
}

/* ---------- nb_toml_get_path ---------- */

static void test_toml_get_path(void)
{
    static const char src[] =
        "[a.b]\n"
        "c = 5\n"
        "[c]\n"
        "\"\" = 7\n"
        "[[arr]]\n"
        "x = 1\n";

    nb_toml *doc = parse_ok(src);
    if (!doc) {
        return;
    }

    nb_str a = NB_STR_LIT("a");
    nb_str b = NB_STR_LIT("b");
    nb_str c = NB_STR_LIT("c");
    nb_str d = NB_STR_LIT("d");
    nb_str arr = NB_STR_LIT("arr");
    nb_str x = NB_STR_LIT("x");
    nb_str missing = NB_STR_LIT("missing");
    nb_str empty = nb_str_from_parts("", 0);

    nb_str path_ab[] = { a, b };
    check_type(nb_toml_get_path(doc, path_ab, 2), NB_TOML_TABLE);

    nb_str path_abc[] = { a, b, c };
    check_int_value(nb_toml_get_path(doc, path_abc, 3), 5);

    nb_str path_abcd[] = { a, b, c, d };
    NB_CHECK(nb_toml_get_path(doc, path_abcd, 4) == NULL);

    nb_str path_a_missing[] = { a, missing };
    NB_CHECK(nb_toml_get_path(doc, path_a_missing, 2) == NULL);

    nb_str path_c[] = { c };
    check_type(nb_toml_get_path(doc, path_c, 1), NB_TOML_TABLE);

    nb_str path_c_empty[] = { c, empty };
    check_int_value(nb_toml_get_path(doc, path_c_empty, 2), 7);

    nb_str path_arr[] = { arr };
    check_type(nb_toml_get_path(doc, path_arr, 1), NB_TOML_ARRAY);

    /* get_path walks named tables only; array elements are explicit. */
    nb_str path_arr_x[] = { arr, x };
    NB_CHECK(nb_toml_get_path(doc, path_arr_x, 2) == NULL);

    nb_toml_free(doc);
}

/* ---------- typed getters ---------- */

static void test_toml_typed_getters(void)
{
    static const char src[] =
        "s = \"text\"\n"
        "i = 1\n"
        "f = 1.5\n"
        "b = true\n"
        "d = 1979-05-27\n"
        "arr = [1]\n"
        "tbl = { x = 1 }\n";

    nb_toml *doc = parse_ok(src);
    if (!doc) {
        return;
    }

    const nb_toml_value *s = doc_get(doc, "s");
    const nb_toml_value *i = doc_get(doc, "i");
    const nb_toml_value *f = doc_get(doc, "f");
    const nb_toml_value *b = doc_get(doc, "b");
    const nb_toml_value *d = doc_get(doc, "d");
    const nb_toml_value *arr = doc_get(doc, "arr");
    const nb_toml_value *tbl = doc_get(doc, "tbl");

    CHECK_STRING(s, "text");
    check_int_value(i, 1);
    check_float_value(f, 1.5);
    check_bool_value(b, true);
    check_datetime_value(d, NB_TOML_LOCAL_DATE, 1979, 5, 27, 0, 0, 0, 0, 0);

    int64_t int_out = 123456789;
    NB_CHECK(!nb_toml_get_integer(s, &int_out));
    NB_CHECK_INT(int_out, 123456789);
    NB_CHECK(!nb_toml_get_integer(f, &int_out));
    NB_CHECK_INT(int_out, 123456789);
    NB_CHECK(!nb_toml_get_integer(b, &int_out));
    NB_CHECK_INT(int_out, 123456789);
    NB_CHECK(!nb_toml_get_integer(d, &int_out));
    NB_CHECK_INT(int_out, 123456789);
    NB_CHECK(!nb_toml_get_integer(arr, &int_out));
    NB_CHECK_INT(int_out, 123456789);
    NB_CHECK(!nb_toml_get_integer(tbl, &int_out));
    NB_CHECK_INT(int_out, 123456789);

    static const char sentinel_text[] = "sentinel";
    nb_str str_out = { sentinel_text, sizeof(sentinel_text) - 1 };
    NB_CHECK(!nb_toml_get_string(i, &str_out));
    NB_CHECK(str_out.ptr == sentinel_text);
    NB_CHECK_INT(str_out.len, sizeof(sentinel_text) - 1);

    double float_out = 2.5;
    NB_CHECK(!nb_toml_get_float(i, &float_out));
    NB_CHECK(float_out == 2.5);

    bool bool_out = true;
    NB_CHECK(!nb_toml_get_bool(s, &bool_out));
    NB_CHECK(bool_out == true);

    nb_toml_datetime dt_out;
    memset(&dt_out, 0, sizeof(dt_out));
    dt_out.year = 1999;
    NB_CHECK(!nb_toml_get_datetime(s, &dt_out));
    NB_CHECK_INT(dt_out.year, 1999);

    /* NULL values must be rejected without touching the output. */
    int_out = 42;
    NB_CHECK(!nb_toml_get_integer(NULL, &int_out));
    NB_CHECK_INT(int_out, 42);

    str_out.ptr = sentinel_text;
    str_out.len = sizeof(sentinel_text) - 1;
    NB_CHECK(!nb_toml_get_string(NULL, &str_out));
    NB_CHECK(str_out.ptr == sentinel_text);
    NB_CHECK_INT(str_out.len, sizeof(sentinel_text) - 1);

    float_out = 3.25;
    NB_CHECK(!nb_toml_get_float(NULL, &float_out));
    NB_CHECK(float_out == 3.25);

    bool_out = false;
    NB_CHECK(!nb_toml_get_bool(NULL, &bool_out));
    NB_CHECK(bool_out == false);

    NB_CHECK(!nb_toml_get_datetime(NULL, &dt_out));
    NB_CHECK_INT(dt_out.year, 1999);

    nb_toml_free(doc);
}

/* ---------- valid document edge cases ---------- */

static void test_toml_valid_documents(void)
{
    nb_toml *doc = parse_ok("");
    if (doc) {
        check_table_size(doc_root(doc), 0);
        check_table_order(doc_root(doc), NULL, 0);
        nb_toml_free(doc);
    }

    doc = parse_ok("# only a comment\n");
    if (doc) {
        check_table_size(doc_root(doc), 0);
        nb_toml_free(doc);
    }

    doc = parse_ok("[empty]\n");
    if (doc) {
        check_table_size(doc_get(doc, "empty"), 0);
        nb_toml_free(doc);
    }

    static const char crlf[] = "a = 1\r\nb = 2\r\n";
    doc = parse_ok_n(crlf, sizeof(crlf) - 1);
    if (doc) {
        check_int_value(doc_get(doc, "a"), 1);
        check_int_value(doc_get(doc, "b"), 2);
        nb_toml_free(doc);
    }

    doc = parse_ok("a = 1");
    if (doc) {
        check_int_value(doc_get(doc, "a"), 1);
        nb_toml_free(doc);
    }

    doc = parse_ok("# comment\n\t\n a\t=\t1 # trailing\r\nb=2\n");
    if (doc) {
        check_int_value(doc_get(doc, "a"), 1);
        check_int_value(doc_get(doc, "b"), 2);
        nb_toml_free(doc);
    }

    static const char bare_keys[] =
        "1234 = 9\n"
        "_ = 10\n"
        "-x = 11\n"
        "\"a.b\" = 12\n"
        "'lit key' = 13\n";
    doc = parse_ok(bare_keys);
    if (doc) {
        check_int_value(doc_get(doc, "1234"), 9);
        check_int_value(doc_get(doc, "_"), 10);
        check_int_value(doc_get(doc, "-x"), 11);
        check_int_value(doc_get(doc, "a.b"), 12);
        check_int_value(doc_get(doc, "lit key"), 13);
        nb_toml_free(doc);
    }
}

/* ---------- TOML 1.1 additions ---------- */

static void test_toml_11_features(void)
{
    static const char src[] =
        "trailing = { x = 1, }\n"
        "multiline = {\n"
        "  a = 1,\n"
        "  b = 2,\n"
        "}\n"
        "commented = {# comment\n"
        "  a = 1,# comment\n"
        "}\n"
        "lt = 13:37\n"
        "ldt = 1979-05-27T07:32\n"
        "odt = 1979-05-27 07:32Z\n"
        "esc = \"\\e\"\n"
        "hex = \"\\x41\"\n"
        "high = \"S\\xf8rmirb\\xe6ren\"\n"
        "nul = \"\\x00\"\n";

    nb_toml *doc = parse_ok(src);
    if (!doc) {
        return;
    }

    check_int_value(table_get(doc_get(doc, "trailing"), "x"), 1);
    check_table_size(doc_get(doc, "multiline"), 2);
    check_int_value(table_get(doc_get(doc, "multiline"), "a"), 1);
    check_int_value(table_get(doc_get(doc, "multiline"), "b"), 2);
    check_int_value(table_get(doc_get(doc, "commented"), "a"), 1);

    check_datetime_value(doc_get(doc, "lt"), NB_TOML_LOCAL_TIME, 0, 0, 0, 13,
                         37, 0, 0, 0);
    check_datetime_value(doc_get(doc, "ldt"), NB_TOML_LOCAL_DATETIME, 1979, 5,
                         27, 7, 32, 0, 0, 0);
    check_datetime_value(doc_get(doc, "odt"), NB_TOML_OFFSET_DATETIME, 1979, 5,
                         27, 7, 32, 0, 0, 0);

    nb_str text;
    NB_CHECK(nb_toml_get_string(doc_get(doc, "esc"), &text));
    NB_CHECK_INT(text.len, 1);
    if (text.len == 1) {
        NB_CHECK_INT((unsigned char)text.ptr[0], 0x1b);
    }

    NB_CHECK(nb_toml_get_string(doc_get(doc, "hex"), &text));
    NB_CHECK_INT(text.len, 1);
    if (text.len == 1) {
        NB_CHECK_INT((unsigned char)text.ptr[0], 'A');
    }

    static const char expected_high[] = "S\xC3\xB8rmirb\xC3\xA6ren";
    NB_CHECK(nb_toml_get_string(doc_get(doc, "high"), &text));
    NB_CHECK_INT(text.len, sizeof(expected_high) - 1u);
    if (text.len == sizeof(expected_high) - 1u) {
        NB_CHECK_MEM(text.ptr, expected_high, text.len);
    }

    NB_CHECK(nb_toml_get_string(doc_get(doc, "nul"), &text));
    NB_CHECK_INT(text.len, 1);
    if (text.len == 1) {
        NB_CHECK_INT((unsigned char)text.ptr[0], 0);
    }

    nb_toml_free(doc);
}

static void test_toml_depth_limit(void)
{
    nb_buf buf;
    nb_buf_init(&buf);

    NB_CHECK(nb_buf_append_cstr(&buf, "a = "));
    for (size_t i = 0; i < 300u; i++) {
        NB_CHECK(nb_buf_append_char(&buf, '['));
    }
    NB_CHECK(nb_buf_append_char(&buf, '0'));
    for (size_t i = 0; i < 300u; i++) {
        NB_CHECK(nb_buf_append_char(&buf, ']'));
    }
    NB_CHECK(nb_buf_append_char(&buf, '\n'));
    expect_parse_error_n(nb_buf_str(&buf).ptr, nb_buf_len(&buf));

    nb_buf_clear(&buf);
    NB_CHECK(nb_buf_append_cstr(&buf, "a = "));
    for (size_t i = 0; i < 300u; i++) {
        NB_CHECK(nb_buf_append_cstr(&buf, "{b="));
    }
    NB_CHECK(nb_buf_append_char(&buf, '0'));
    for (size_t i = 0; i < 300u; i++) {
        NB_CHECK(nb_buf_append_char(&buf, '}'));
    }
    NB_CHECK(nb_buf_append_char(&buf, '\n'));
    expect_parse_error_n(nb_buf_str(&buf).ptr, nb_buf_len(&buf));

    nb_buf_clear(&buf);
    NB_CHECK(nb_buf_append_char(&buf, '['));
    for (size_t i = 0; i < 300u; i++) {
        NB_CHECK(nb_buf_append_cstr(&buf, "a."));
    }
    NB_CHECK(nb_buf_append_cstr(&buf, "a]\n"));
    expect_parse_error_n(nb_buf_str(&buf).ptr, nb_buf_len(&buf));

    nb_buf_destroy(&buf);
}

static void test_toml_locale_float(void)
{
    const char *current = setlocale(LC_NUMERIC, NULL);
    char saved[128];
    saved[0] = '\0';
    if (current) {
        snprintf(saved, sizeof(saved), "%s", current);
    }
    const char *changed = setlocale(LC_NUMERIC, "de_DE.UTF-8");
    if (!changed) {
        changed = setlocale(LC_NUMERIC, "de_DE");
    }
    if (!changed) {
        if (saved[0] != '\0') {
            setlocale(LC_NUMERIC, saved);
        }
        return;
    }

    nb_toml *doc = parse_ok("a = 1.5\nb = 1.5e2\n");
    if (doc) {
        check_float_value(doc_get(doc, "a"), 1.5);
        check_float_value(doc_get(doc, "b"), 150.0);
        nb_toml_free(doc);
    }
    setlocale(LC_NUMERIC, saved[0] != '\0' ? saved : "C");
}

/* ---------- invalid documents ---------- */

static void test_toml_invalid_documents(void)
{
    static const char *const cases[] = {
        /* Duplicate keys. */
        "a = 1\na = 2\n",
        "[a]\nb = 1\nb = 2\n",
        "a.b = 1\na.b = 2\n",
        "a = { x = 1, x = 2 }\n",
        "\"\" = 1\n\"\" = 2\n",

        /* Duplicate or conflicting tables. */
        "[a]\nx = 1\n[a]\ny = 2\n",
        "[a.b]\n[a.b]\n",
        "[a]\n[a.b]\n[a]\n",
        "a = 1\n[a.b]\n",
        "a = 1\na.b = 2\n",
        "a = 1\n[a]\n",
        "a.b = 1\na = 2\n",
        "a = { x = 1 }\na.y = 2\n",
        "a = { x = 1 }\n[a.b]\n",

        /* Array-of-tables misuse. */
        "a = 1\n[[a]]\n",
        "a = []\n[[a]]\n",
        "[a]\n[[a]]\n",
        "[[a]]\n[a]\n",
        "a = { x = 1 }\n[[a]]\n",

        /* Inline table syntax. */
        "a = { x = 1\n",
        "a = { x = 1,\n",

        /* Integers. */
        "a = 01\n",
        "a = -01\n",
        "a = +01\n",
        "a = 1__0\n",
        "a = _1\n",
        "a = 1_\n",
        "a = 0x_1\n",
        "a = 0x1_\n",
        "a = 0o8\n",
        "a = 0o_7\n",
        "a = 0b2\n",
        "a = 0b1_\n",
        "a = 9223372036854775808\n",
        "a = -9223372036854775809\n",
        "a = 0x10000000000000000\n",

        /* Floats. */
        "a = 1.\n",
        "a = .5\n",
        "a = 1e\n",
        "a = 1e+\n",
        "a = 01.0\n",
        "a = 1_.0\n",
        "a = 1._0\n",
        "a = 1e1__0\n",
        "a = NaN\n",
        "a = Infinity\n",

        /* Dates and times. */
        "a = 2023-02-29\n",
        "a = 1900-02-29\n",
        "a = 2023-13-01\n",
        "a = 2023-00-10\n",
        "a = 2023-04-31\n",
        "a = 2023-01-32\n",
        "a = 24:00:00\n",
        "a = 07:60:00\n",
        "a = 07:32:61\n",
        "a = 1979-05-27T07:32:00+24:00\n",
        "a = 1979-05-27T07:32:00+05:60\n",
        "a = 1979-5-27\n",
        "a = 1979-05-27T7:32:00\n",
        "a = 2023-02-29T00:00:00Z\n",

        /* Escapes. */
        "a = \"\\q\"\n",
        "a = \"\\u12\"\n",
        "a = \"\\u00G0\"\n",
        "a = \"\\uD800\"\n",
        "a = \"\\U0000D800\"\n",
        "a = \"\\U00110000\"\n",
        "a = \"\\U0001F60\"\n",

        /* Strings and line endings. */
        "a = \"abc\n",
        "a = 'abc\n",
        "a = \"\"\"abc\n",
        "a = '''abc\n",
        "a = \"line1\nline2\"\n",
        "a = 'line1\nline2'\n",
        "a = \"\\\n",

        /* Arrays. */
        "a = [1, 2\n",
        "a = [1,,2]\n",
        "a = [,]\n",
        "a = [1 2]\n",
        "a = [1, 2]]\n",
        "a = [[1]\n",

        /* Trailing garbage. */
        "a = 1 b\n",
        "a = 1 2\n",
        "a = 1 b = 2\n",
        "a = truex\n",
        "a = \"x\" \"y\"\n",
        "a = [1] [2]\n",
        "[a] x\n",
        "[a]b = 1\n",

        /* Keys and headers. */
        "a b = 1\n",
        "= 1\n",
        "a =\n",
        "[]\n",
        "[a\n",
        "[a.]\n",
        ".a = 1\n",
        "a..b = 1\n",
        "a. = 1\n",
        "\"a = 1\n",
        "a = 1 b = 2\n",
    };

    for (size_t i = 0; i < NB_ARRAY_SIZE(cases); i++) {
        expect_parse_error(cases[i]);
    }

    /* Bare CR is not a line ending and is not whitespace. */
    expect_parse_error("a = 1\rb = 2\n");
    expect_parse_error("a = 1\r");

    /* NUL bytes in the source are invalid, including inside strings. */
    static const char nul_between[] = {
        'a', ' ', '=', ' ', '1', '\0', 'b', ' ', '=', ' ', '2', '\n'
    };
    expect_parse_error_n(nul_between, sizeof(nul_between));

    static const char nul_inside_string[] = {
        'a', ' ', '=', ' ', '"', 'x', '\0', 'y', '"', '\n'
    };
    expect_parse_error_n(nul_inside_string, sizeof(nul_inside_string));

    static const char trailing_nul[] = { 'a', ' ', '=', ' ', '1', '\0' };
    expect_parse_error_n(trailing_nul, sizeof(trailing_nul));

    /* Invalid UTF-8 bytes. */
    static const char bad_utf8_raw[] = {
        'a', ' ', '=', ' ', '"', (char)0xFF, '"', '\n'
    };
    expect_parse_error_n(bad_utf8_raw, sizeof(bad_utf8_raw));

    static const char bad_utf8_truncated[] = {
        'a', ' ', '=', ' ', '"', (char)0xE4, (char)0xB8, '"', '\n'
    };
    expect_parse_error_n(bad_utf8_truncated, sizeof(bad_utf8_truncated));

    static const char bad_utf8_overlong[] = {
        'a', ' ', '=', ' ', '"', (char)0xC0, (char)0x80, '"', '\n'
    };
    expect_parse_error_n(bad_utf8_overlong, sizeof(bad_utf8_overlong));

    static const char non_ascii_bare_key[] = {
        (char)0xC3, (char)0xA9, ' ', '=', ' ', '1', '\n'
    };
    expect_parse_error_n(non_ascii_bare_key, sizeof(non_ascii_bare_key));

    /* Raw control characters must be escaped in basic strings. */
    static const char ctrl_in_string[] = {
        'a', ' ', '=', ' ', '"', 'x', '\x01', 'y', '"', '\n'
    };
    expect_parse_error_n(ctrl_in_string, sizeof(ctrl_in_string));

    static const char del_in_string[] = {
        'a', ' ', '=', ' ', '"', 'x', '\x7f', 'y', '"', '\n'
    };
    expect_parse_error_n(del_in_string, sizeof(del_in_string));
}

static void test_toml_source_and_equivalence(void)
{
    const char *text = "# prefix\n[owner]\nname = \"é\"\ninline = { key = 12 }\n"
                       "array = [1, 2]\nwhen = 2025-04-03T12:34:56.123456789+02:30\n"
                       "nul = \"a\\u0000b\"\n";
    nb_toml *doc = parse_ok(text);
    if (!doc) {
        return;
    }
    const nb_toml_value *owner = nb_toml_table_get_cstr(nb_toml_root(doc), "owner");
    nb_toml_location header = nb_toml_location_of(owner);
    NB_CHECK_INT(header.line, 2);
    NB_CHECK(header.table_header);
    NB_CHECK_MEM(text + header.start, "[owner]", header.end - header.start);
    const nb_toml_value *name = nb_toml_table_get_cstr(owner, "name");
    nb_toml_location loc = nb_toml_location_of(name);
    NB_CHECK_INT(loc.line, 3);
    NB_CHECK_INT(loc.column, 8);
    NB_CHECK_MEM(text + loc.start, "\"é\"", loc.end - loc.start);
    NB_CHECK_STR(nb_toml_string(name).ptr, "é");
    const nb_toml_value *inlined = nb_toml_table_get_cstr(owner, "inline");
    NB_CHECK(nb_toml_location_of(inlined).inline_table);
    NB_CHECK(!nb_toml_location_of(inlined).table_header);
    const nb_toml_value *nul = nb_toml_table_get_cstr(owner, "nul");
    NB_CHECK_INT(nb_toml_string(nul).len, 3);
    NB_CHECK_MEM(nb_toml_string(nul).ptr, "a\0b", 3);
    NB_CHECK_STR(nb_toml_table_key_at(owner, 0).ptr, "name");
    NB_CHECK(nb_toml_table_value_at(owner, 0) == name);
    NB_CHECK(nb_toml_table_value_at(owner, SIZE_MAX) == NULL);
    NB_CHECK(nb_toml_table_key_at(owner, SIZE_MAX).ptr == NULL);
    nb_toml *same = parse_ok(text);
    NB_CHECK(nb_toml_value_equal(nb_toml_root(doc), nb_toml_root(same)));
    nb_toml *changed = parse_ok("[owner]\nname = \"different\"\n");
    NB_CHECK(!nb_toml_value_equal(nb_toml_root(doc), nb_toml_root(changed)));
    nb_toml_datetime date = {0};
    NB_CHECK(nb_toml_get_datetime(nb_toml_table_get_cstr(owner, "when"), &date));
    NB_CHECK_INT(date.nanosecond, 123456789);
    NB_CHECK_INT(date.offset_minutes, 150);
    nb_toml_free(changed);
    nb_toml_free(same);
    nb_toml_free(doc);
}

void test_toml(void)
{
    test_toml_source_and_equivalence();
    test_toml_strings();
    test_toml_integers();
    test_toml_floats();
    test_toml_booleans();
    test_toml_datetimes();
    test_toml_tables();
    test_toml_quoted_and_dotted_keys();
    test_toml_arrays();
    test_toml_inline_tables();
    test_toml_arrays_of_tables();
    test_toml_iteration_order();
    test_toml_get_path();
    test_toml_typed_getters();
    test_toml_valid_documents();
    test_toml_11_features();
    test_toml_invalid_documents();
    test_toml_depth_limit();
    test_toml_locale_float();
}
