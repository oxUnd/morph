#include "test.h"

#include "nb/file.h"
#include "nb/str.h"
#include "nb/time.h"

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static bool make_temp_dir(nb_buf *dir, nb_err *err)
{
    if (nb_file_temp(dir, err) != NB_OK) {
        return false;
    }
    if (nb_file_remove(nb_buf_cstr(dir), err) != NB_OK) {
        return false;
    }
    return nb_file_mkdir(nb_buf_cstr(dir), false, err) == NB_OK;
}

static int count_dir_entries(const char *dir)
{
    nb_err err;
    nb_err_clear(&err);
    nb_dir *handle = nb_dir_open(dir, &err);
    if (!handle) {
        return -1;
    }
    int count = 0;
    while (nb_dir_next(handle, NULL)) {
        count++;
    }
    nb_dir_close(handle);
    return count;
}

static void remove_dir(const char *path)
{
    nb_err err;
    nb_err_clear(&err);
    (void)nb_file_rmdir(path, &err);
}

static void test_atomic_write(void)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *dir = nb_buf_new();
    nb_buf *file = nb_buf_new();
    nb_buf *data = nb_buf_new();

    bool ready = make_temp_dir(dir, &err);
    NB_CHECK(ready);
    if (!ready) {
        nb_buf_free(data);
        nb_buf_free(file);
        nb_buf_free(dir);
        return;
    }

    NB_CHECK_INT(nb_path_join(file, nb_buf_cstr(dir), "payload.bin"), NB_OK);

    static const char first[] = "first payload\n";
    nb_err_clear(&err);
#ifndef _WIN32
    mode_t previous = umask(022);
#endif
    nb_status status = nb_file_write_all_atomic(
        nb_buf_cstr(file), first, sizeof(first) - 1u, NULL, &err);
#ifndef _WIN32
    (void)umask(previous);
#endif
    NB_CHECK_INT(status, NB_OK);
    NB_CHECK_INT(err.code, NB_OK);

#ifndef _WIN32
    struct stat info = { 0 };
    NB_CHECK_INT(stat(nb_buf_cstr(file), &info), 0);
    NB_CHECK(S_ISREG(info.st_mode));
    NB_CHECK_INT((long long)(info.st_mode & 0777), 0644);
#endif

    NB_CHECK_INT(nb_file_read_all(nb_buf_cstr(file), data, 0, &err), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(data), first);

    static const char second[] = "second\n";
    nb_err_clear(&err);
    NB_CHECK_INT(nb_file_write_all_atomic(
                     nb_buf_cstr(file), second, sizeof(second) - 1u, NULL,
                     &err),
                 NB_OK);
    NB_CHECK_INT(nb_file_read_all(nb_buf_cstr(file), data, 0, &err), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(data), second);

#ifndef _WIN32
    NB_CHECK_INT(chmod(nb_buf_cstr(file), 0600), 0);
    previous = umask(077);
#endif
    nb_err_clear(&err);
    status = nb_file_write_all_atomic(
        nb_buf_cstr(file), second, sizeof(second) - 1u, NULL, &err);
#ifndef _WIN32
    (void)umask(previous);
    NB_CHECK_INT(status, NB_OK);
    NB_CHECK_INT(stat(nb_buf_cstr(file), &info), 0);
    NB_CHECK_INT((long long)(info.st_mode & 0777), 0600);
#else
    NB_CHECK_INT(status, NB_OK);
#endif

    nb_err_clear(&err);
    NB_CHECK_INT(nb_file_write_all_atomic(nb_buf_cstr(file), NULL, 0, NULL,
                                          &err),
                 NB_OK);
    NB_CHECK_INT(nb_file_read_all(nb_buf_cstr(file), data, 0, &err), NB_OK);
    NB_CHECK_INT(nb_buf_len(data), 0);

    NB_CHECK_INT(count_dir_entries(nb_buf_cstr(dir)), 1);

    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(file), &err), NB_OK);
    NB_CHECK_INT(nb_file_rmdir(nb_buf_cstr(dir), &err), NB_OK);
    nb_buf_free(data);
    nb_buf_free(file);
    nb_buf_free(dir);
}

static void test_atomic_write_failure(void)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *dir = nb_buf_new();
    nb_buf *file = nb_buf_new();
    nb_buf *missing = nb_buf_new();
    nb_buf *bad = nb_buf_new();
    nb_buf *blocked = nb_buf_new();

    bool ready = make_temp_dir(dir, &err);
    NB_CHECK(ready);
    if (!ready) {
        nb_buf_free(blocked);
        nb_buf_free(bad);
        nb_buf_free(missing);
        nb_buf_free(file);
        nb_buf_free(dir);
        return;
    }

    NB_CHECK_INT(nb_path_join(file, nb_buf_cstr(dir), "target.bin"), NB_OK);

    NB_CHECK_INT(nb_file_write_all_atomic(NULL, "x", 1, NULL, &err),
                 NB_ERR_INVAL);
    NB_CHECK_INT(nb_file_write_all_atomic("", "x", 1, NULL, &err),
                 NB_ERR_INVAL);
    NB_CHECK_INT(nb_file_write_all_atomic(nb_buf_cstr(file), NULL, 1, NULL,
                                          &err),
                 NB_ERR_INVAL);
    NB_CHECK(!nb_file_exists(nb_buf_cstr(file)));
    NB_CHECK_INT(count_dir_entries(nb_buf_cstr(dir)), 0);

    NB_CHECK_INT(nb_path_join(missing, nb_buf_cstr(dir), "missing"), NB_OK);
    NB_CHECK_INT(nb_path_join(bad, nb_buf_cstr(missing), "child"), NB_OK);
    nb_err_clear(&err);
    NB_CHECK_INT(nb_file_write_all_atomic(nb_buf_cstr(bad), "x", 1, NULL,
                                          &err),
                 NB_ERR_IO);
    NB_CHECK_INT(err.code, NB_ERR_IO);
#ifdef _WIN32
    NB_CHECK(err.os_errno != 0);
#else
    NB_CHECK_INT(err.os_errno, ENOENT);
#endif
    NB_CHECK(!nb_file_exists(nb_buf_cstr(bad)));
    NB_CHECK_INT(count_dir_entries(nb_buf_cstr(dir)), 0);

    NB_CHECK_INT(nb_path_join(blocked, nb_buf_cstr(dir), "blocked"), NB_OK);
    NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(blocked), false, &err), NB_OK);
    nb_err_clear(&err);
    NB_CHECK_INT(nb_file_write_all_atomic(nb_buf_cstr(blocked), "data", 4,
                                          NULL, &err),
                 NB_ERR_IO);
    NB_CHECK_INT(err.code, NB_ERR_IO);
    NB_CHECK(err.os_errno != 0);
    NB_CHECK(nb_file_is_dir(nb_buf_cstr(blocked)));
    NB_CHECK_INT(count_dir_entries(nb_buf_cstr(dir)), 1);
    NB_CHECK_INT(count_dir_entries(nb_buf_cstr(blocked)), 0);

    NB_CHECK_INT(nb_file_rmdir(nb_buf_cstr(blocked), &err), NB_OK);
    NB_CHECK_INT(nb_file_rmdir(nb_buf_cstr(dir), &err), NB_OK);
    nb_buf_free(blocked);
    nb_buf_free(bad);
    nb_buf_free(missing);
    nb_buf_free(file);
    nb_buf_free(dir);
}

static void test_path_clean(void)
{
    static const struct {
        const char *input;
        const char *expected;
    } cases[] = {
        { "/", "/" },
        { "///", "/" },
        { "/./", "/" },
        { "/..", "/" },
        { "/../../a", "/a" },
        { "/a//b/./c/../", "/a/b" },
        { "//a///b//", "/a/b" },
        { "/a/b/../..", "/" },
        { "a//b/./c/../", "a/b" },
        { "a/b/../../c", "c" },
        { "a//..//b", "b" },
        { "a/../../b", "../b" },
        { "../..", "../.." },
        { "../../../a", "../../../a" },
        { "../a/..", ".." },
        { ".", "." },
        { "./", "." },
        { "a/..", "." },
        { "a/../..", ".." },
        { "a/b/..", "a" },
        { "...", "..." },
        { ".hidden/./x", ".hidden/x" },
        { "a/.../b", "a/.../b" },
        { "..hidden", "..hidden" },
        { "a/b/../", "a" },
#ifdef _WIN32
        { "C:\\a\\..\\b", "C:/b" },
        { "C:\\a\\b\\..", "C:/a" },
        { "C:foo\\..\\bar", "C:bar" },
        { "C:..\\x", "C:../x" },
        { "C:foo\\..\\..\\bar", "C:../bar" },
        { "C:", "C:" },
        { "C:.", "C:" },
        { "\\\\server\\share\\a\\..\\b", "//server/share/b" },
#endif
    };

    nb_buf *out = nb_buf_new();
    for (size_t i = 0; i < NB_ARRAY_SIZE(cases); i++) {
        NB_CHECK(nb_buf_append_cstr(out, "stale"));
        NB_CHECK_INT(nb_path_clean(out, cases[i].input), NB_OK);
        NB_CHECK_STR(nb_buf_cstr(out), cases[i].expected);
    }

    NB_CHECK_INT(nb_path_clean(out, ""), NB_ERR_INVAL);
    NB_CHECK_INT(nb_path_clean(NULL, "a"), NB_ERR_INVAL);
    NB_CHECK_INT(nb_path_clean(out, NULL), NB_ERR_INVAL);
    nb_buf_free(out);
}

static void check_path_split(const char *path, const char *expected_dir,
                             const char *expected_name)
{
    nb_str dir = NB_STR_NULL;
    nb_str name = NB_STR_NULL;
    if (nb_path_split(path, &dir, &name) != NB_OK) {
        NB_CHECK(false);
        return;
    }
    NB_CHECK_INT(dir.len, strlen(expected_dir));
    if (dir.len == strlen(expected_dir)) {
        NB_CHECK_MEM(dir.ptr, expected_dir, dir.len);
    }
    NB_CHECK_INT(name.len, strlen(expected_name));
    if (name.len == strlen(expected_name)) {
        NB_CHECK_MEM(name.ptr, expected_name, name.len);
    }
}

static void test_path_split(void)
{
    static const struct {
        const char *input;
        const char *dir;
        const char *name;
    } cases[] = {
        { "/a/b/c.txt", "/a/b", "c.txt" },
        { "/a/b/", "/a", "b" },
        { "/a", "/", "a" },
        { "/", "/", "" },
        { "///", "/", "" },
        { "a/b", "a", "b" },
        { "a", "", "a" },
        { "./a", ".", "a" },
        { "../a", "..", "a" },
        { "a//b", "a", "b" },
        { "a/..", "a", ".." },
        { "..", "", ".." },
#ifdef _WIN32
        { "C:\\a\\b", "C:\\a", "b" },
        { "C:\\", "C:\\", "" },
        { "C:foo", "C:", "foo" },
        { "C:..\\x", "C:..", "x" },
#endif
    };

    for (size_t i = 0; i < NB_ARRAY_SIZE(cases); i++) {
        check_path_split(cases[i].input, cases[i].dir, cases[i].name);
    }
    NB_CHECK_INT(nb_path_split("a/b", NULL, NULL), NB_OK);

    nb_str dir = NB_STR_LIT("stale");
    nb_str name = NB_STR_LIT("stale");
    NB_CHECK_INT(nb_path_split("", &dir, &name), NB_ERR_INVAL);
    NB_CHECK_INT(nb_path_split(NULL, &dir, &name), NB_ERR_INVAL);
    NB_CHECK_INT(dir.len, 5);
    NB_CHECK_INT(name.len, 5);

#ifdef _WIN32
    nb_buf *joined = nb_buf_new();
    NB_CHECK_INT(nb_path_join(joined, "C:", "foo"), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(joined), "C:foo");
    nb_buf_free(joined);
#endif
}

static void test_path_dirname(void)
{
    static const struct {
        const char *input;
        const char *expected;
    } cases[] = {
        { "/a/b/c.txt", "/a/b" },
        { "/a/b/", "/a" },
        { "/a", "/" },
        { "/", "/" },
        { "a", "." },
        { "a/b", "a" },
        { "a//b", "a" },
        { "./a", "." },
        { "../a", ".." },
        { "..", "." },
        { "a/..", "a" },
#ifdef _WIN32
        { "C:\\a\\b", "C:\\a" },
        { "C:\\", "C:\\" },
        { "C:foo", "C:" },
        { "C:..\\x", "C:.." },
#endif
    };

    nb_buf *out = nb_buf_new();
    for (size_t i = 0; i < NB_ARRAY_SIZE(cases); i++) {
        nb_buf_clear(out);
        NB_CHECK(nb_buf_append_cstr(out, "stale"));
        NB_CHECK_INT(nb_path_dirname(out, cases[i].input), NB_OK);
        NB_CHECK_STR(nb_buf_cstr(out), cases[i].expected);
    }

    NB_CHECK_INT(nb_path_dirname(NULL, "a/b"), NB_ERR_INVAL);
    NB_CHECK_INT(nb_path_dirname(out, ""), NB_ERR_INVAL);
    NB_CHECK_INT(nb_path_dirname(out, NULL), NB_ERR_INVAL);
    NB_CHECK_STR(nb_buf_cstr(out), cases[NB_ARRAY_SIZE(cases) - 1].expected);

    nb_buf_clear(out);
    NB_CHECK(nb_buf_append_cstr(out, "a/b"));
    NB_CHECK_INT(nb_path_dirname(out, nb_buf_cstr(out)), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(out), "a");
    nb_buf_free(out);
}

static void test_path_set_extension(void)
{
    static const struct {
        const char *input;
        const char *extension;
        const char *expected;
    } cases[] = {
        { "/a/b.txt", "zip", "/a/b.zip" },
        { "/a/b.tar.gz", "zip", "/a/b.tar.zip" },
        { "/a/b", "txt", "/a/b.txt" },
        { "/a/.hidden", "txt", "/a/.hidden.txt" },
        { "/a/b.txt", NULL, "/a/b" },
        { "/a/b.txt", "", "/a/b" },
        { "/a/b.", "txt", "/a/b.txt" },
        { "/a/b.txt", ".zip", "/a/b.zip" },
        { "/a/b.txt", "..zip", "/a/b.zip" },
        { "b.txt", "tar.gz", "b.tar.gz" },
        { ".hidden", "txt", ".hidden.txt" },
        { "dir/a.b/c", "txt", "dir/a.b/c.txt" },
    };

    nb_buf *out = nb_buf_new();
    for (size_t i = 0; i < NB_ARRAY_SIZE(cases); i++) {
        nb_buf_clear(out);
        NB_CHECK(nb_buf_append_cstr(out, "stale"));
        NB_CHECK_INT(nb_path_set_extension(out, cases[i].input,
                                           cases[i].extension),
                     NB_OK);
        NB_CHECK_STR(nb_buf_cstr(out), cases[i].expected);
    }

    nb_buf_clear(out);
    NB_CHECK(nb_buf_append_cstr(out, "a/b.txt"));
    NB_CHECK_INT(nb_path_set_extension(out, nb_buf_cstr(out), "md"), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(out), "a/b.md");

    static const struct {
        const char *input;
        const char *extension;
    } invalid[] = {
        { "", "txt" },
        { NULL, "txt" },
        { "/", "txt" },
        { "/a/", "txt" },
        { "a/", "txt" },
        { ".", "txt" },
        { "..", "txt" },
        { "a/b", "x/y" },
#ifdef _WIN32
        { "a/b", "x\\y" },
#endif
    };
    for (size_t i = 0; i < NB_ARRAY_SIZE(invalid); i++) {
        nb_buf_clear(out);
        NB_CHECK(nb_buf_append_cstr(out, "stale"));
        NB_CHECK_INT(nb_path_set_extension(out, invalid[i].input,
                                           invalid[i].extension),
                     NB_ERR_INVAL);
        NB_CHECK_STR(nb_buf_cstr(out), "stale");
    }
    NB_CHECK_INT(nb_path_set_extension(NULL, "a", "txt"), NB_ERR_INVAL);
    nb_buf_free(out);
}

static void test_path_is_absolute(void)
{
    NB_CHECK(nb_path_is_absolute("/"));
    NB_CHECK(nb_path_is_absolute("/a/b"));
    NB_CHECK(!nb_path_is_absolute("a/b"));
    NB_CHECK(!nb_path_is_absolute(""));
    NB_CHECK(!nb_path_is_absolute(NULL));
#ifdef _WIN32
    NB_CHECK(nb_path_is_absolute("C:\\a"));
    NB_CHECK(nb_path_is_absolute("\\\\server\\share"));
    NB_CHECK(!nb_path_is_absolute("C:a"));
#endif
}

static void test_path_absolute_long_cwd(void)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *base = nb_buf_new();
    bool ready = make_temp_dir(base, &err);
    NB_CHECK(ready);
    if (!ready) {
        nb_buf_free(base);
        return;
    }

    char saved[8192];
    if (!getcwd(saved, sizeof(saved))) {
        NB_CHECK(false);
        remove_dir(nb_buf_cstr(base));
        nb_buf_free(base);
        return;
    }

    nb_buf *passthrough = nb_buf_new();
    nb_err_clear(&err);
    NB_CHECK_INT(nb_path_absolute(passthrough, "/already/absolute", &err),
                 NB_OK);
    NB_CHECK_STR(nb_buf_cstr(passthrough), "/already/absolute");
    nb_buf_free(passthrough);

#ifdef _WIN32
    enum { LEVELS = 1 };
#else
    enum { LEVELS = 4 };
#endif
    char names[LEVELS][201];
    for (int i = 0; i < LEVELS; i++) {
        memset(names[i], 'a' + i, 200);
        names[i][200] = '\0';
    }

    bool deep = chdir(nb_buf_cstr(base)) == 0;
    for (int i = 0; deep && i < LEVELS; i++) {
        nb_err_clear(&err);
        if (nb_file_mkdir(names[i], false, &err) != NB_OK ||
            chdir(names[i]) != 0) {
            deep = false;
        }
    }

    if (deep) {
        nb_buf *absolute = nb_buf_new();
        nb_err_clear(&err);
        NB_CHECK_INT(nb_path_absolute(absolute, "leaf", &err), NB_OK);

        char expected[8192];
        if (getcwd(expected, sizeof(expected))) {
#ifdef _WIN32
            for (char *p = expected; *p; p++) {
                if (*p == '\\') {
                    *p = '/';
                }
            }
#endif
            size_t expected_len = strlen(expected);
            NB_CHECK_INT(nb_buf_len(absolute),
                         (long long)expected_len + 5);
            if (nb_buf_len(absolute) == expected_len + 5u) {
                NB_CHECK(strncmp(nb_buf_cstr(absolute), expected,
                                 expected_len) == 0);
                NB_CHECK_STR(nb_buf_cstr(absolute) + expected_len, "/leaf");
            }
        } else {
            NB_CHECK(false);
        }
        nb_buf_free(absolute);
    }

    /* Remove the deep tree with relative names so cleanup does not depend on
     * the temporary directory's absolute length. */
    if (chdir(nb_buf_cstr(base)) == 0) {
        char relative[LEVELS * 201 + 1];
        for (int depth = LEVELS - 1; depth >= 0; depth--) {
            relative[0] = '\0';
            for (int i = 0; i <= depth; i++) {
                if (i != 0) {
                    strcat(relative, "/");
                }
                strcat(relative, names[i]);
            }
            remove_dir(relative);
        }
    }
    NB_CHECK_INT(chdir(saved), 0);
    NB_CHECK_INT(nb_file_rmdir(nb_buf_cstr(base), &err), NB_OK);
    nb_buf_free(base);
}

static void test_time_format_utc(void)
{
    const int64_t sample = INT64_C(1794183240123000000);
    char text[64];

    NB_CHECK(nb_time_format_rfc3339_utc(sample, text, sizeof(text)));
    NB_CHECK_STR(text, "2026-11-09T00:14:00.123Z");

    char exact[25];
    NB_CHECK(!nb_time_format_rfc3339_utc(sample, exact, 24));
    NB_CHECK(nb_time_format_rfc3339_utc(sample, exact, sizeof(exact)));
    NB_CHECK_STR(exact, "2026-11-09T00:14:00.123Z");

    NB_CHECK(!nb_time_format_rfc3339_utc(sample, NULL, sizeof(text)));
    NB_CHECK(!nb_time_format_rfc3339_utc(sample, text, 0));

    NB_CHECK(nb_time_format_rfc3339_utc(0, text, sizeof(text)));
    NB_CHECK_STR(text, "1970-01-01T00:00:00.000Z");

    NB_CHECK(nb_time_format_rfc3339_utc(-1, text, sizeof(text)));
    NB_CHECK_STR(text, "1969-12-31T23:59:59.999Z");

    NB_CHECK(nb_time_format_rfc3339_utc(INT64_C(-1000000001), text,
                                        sizeof(text)));
    NB_CHECK_STR(text, "1969-12-31T23:59:58.999Z");

    int64_t parsed = 0;
    NB_CHECK(nb_time_parse_rfc3339("1969-12-31T23:59:59.999Z", &parsed));
    NB_CHECK_INT(parsed, INT64_C(-1000000));
}

static void check_parse_valid(const char *text, int64_t expected)
{
    int64_t value = 0;
    NB_CHECK(nb_time_parse_rfc3339(text, &value));
    NB_CHECK_INT(value, expected);
}

static void check_parse_invalid(const char *text)
{
    int64_t value = 0;
    NB_CHECK(!nb_time_parse_rfc3339(text, &value));
}

static void test_time_parse(void)
{
    const int64_t sample = INT64_C(1794183240123000000);

    check_parse_valid("1970-01-01T00:00:00Z", 0);
    check_parse_valid("2026-11-09T00:14:00.123Z", sample);
    check_parse_valid("2026-11-09T00:14:00.123z", sample);
    check_parse_valid("2026-11-09T08:14:00.123+08:00", sample);
    check_parse_valid("2026-11-08T16:14:00.123-08:00", sample);
    check_parse_valid("2026-11-09T00:14:00.1Z",
                      INT64_C(1794183240100000000));
    check_parse_valid("2026-11-09T00:14:00.123456789Z",
                      INT64_C(1794183240123456789));
    check_parse_valid("2026-11-09T00:14:00.123456789999Z",
                      INT64_C(1794183240123456789));
    check_parse_valid("2024-02-29T00:00:00Z",
                      INT64_C(1709164800000000000));
    check_parse_valid("2016-12-31T23:59:60Z",
                      INT64_C(1483228800000000000));

    int64_t value = 0;
    NB_CHECK(!nb_time_parse_rfc3339(NULL, &value));
    NB_CHECK(!nb_time_parse_rfc3339("2026-11-09T00:14:00Z", NULL));

    static const char *const invalid[] = {
        "",
        "2026-+1-01T00:00:00Z",
        "2026-1-01T00:00:00Z",
        "2026-01-1T00:00:00Z",
        "2026-01-01T0:00:00Z",
        "2026-01-01T00:0:00Z",
        "2026-01-01T00:00:0Z",
        "2026-01-01X00:00:00Z",
        "2026-01-01t00:00:00Z",
        "2026-01-01T00;00:00Z",
        "2026-01-01T00:00;00Z",
        "2026/01/01T00:00:00Z",
        "abcd-01-01T00:00:00Z",
        "2026-01-01T00:00:00",
        "2026-01-01T00:00:00.Z",
        "2026-01-01T00:00:00,123Z",
        "2026-01-01T00:00:00.12aZ",
        "2026-01-01T00:00:00+0:00",
        "2026-01-01T00:00:00+00:0",
        "2026-01-01T00:00:00+0000",
        "2026-01-01T00:00:00+00",
        "2026-01-01T00:00:00+24:00",
        "2026-01-01T00:00:00+00:60",
        "2026-01-01T00:00:00Zx",
        " 2026-01-01T00:00:00Z",
        "2026-01-01T00:00:00Z ",
        "2026-02-30T00:00:00Z",
        "2026-13-01T00:00:00Z",
        "2026-00-01T00:00:00Z",
        "2026-01-00T00:00:00Z",
        "2026-01-01T24:00:00Z",
        "2026-01-01T00:60:00Z",
        "2026-01-01T00:00:61Z",
        "2025-02-29T00:00:00Z",
        "0000-01-01T00:00:00Z",
    };
    for (size_t i = 0; i < NB_ARRAY_SIZE(invalid); i++) {
        check_parse_invalid(invalid[i]);
    }

    /* Syntactically valid, but outside the int64_t nanosecond range. */
    check_parse_invalid("9999-12-31T23:59:59Z");

    char text[32];
    NB_CHECK(nb_time_format_rfc3339_utc(sample, text, sizeof(text)));
    NB_CHECK(nb_time_parse_rfc3339(text, &value));
    NB_CHECK_INT(value, sample);
}

void test_file_time(void)
{
    test_atomic_write();
    test_atomic_write_failure();
    test_path_clean();
    test_path_split();
    test_path_dirname();
    test_path_set_extension();
    test_path_is_absolute();
    test_path_absolute_long_cwd();
    test_time_format_utc();
    test_time_parse();
}
