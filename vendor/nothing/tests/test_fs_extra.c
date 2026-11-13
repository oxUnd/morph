#include "test.h"

#include "nb/buf.h"
#include "nb/file.h"
#include "nb/str.h"
#include "nb/random.h"

#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static void remove_tree(const char *path)
{
    nb_err err;
    nb_err_clear(&err);
    nb_dir *dir = nb_dir_open(path, &err);
    if (dir) {
        for (;;) {
            nb_dir_entry entry;
            const char *name = nb_dir_next(dir, &entry);
            if (!name) {
                break;
            }
            nb_buf *child = nb_buf_new();
            if (!child) {
                break;
            }
            if (nb_path_join(child, path, name) != NB_OK) {
                nb_buf_free(child);
                continue;
            }
            if (entry.type == NB_DIR_ENTRY_DIR) {
                remove_tree(nb_buf_cstr(child));
            } else if (entry.type == NB_DIR_ENTRY_UNKNOWN) {
                /* Do not follow an unresolved symlink into its target. */
                if (nb_file_remove(nb_buf_cstr(child), &err) != NB_OK &&
                    nb_file_is_dir(nb_buf_cstr(child))) {
                    remove_tree(nb_buf_cstr(child));
                }
            } else {
                (void)nb_file_remove(nb_buf_cstr(child), &err);
            }
            nb_buf_free(child);
        }
        nb_dir_close(dir);
    }
    (void)nb_file_rmdir(path, &err);
}

static void check_contents(const char *path, const char *expected)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *data = nb_buf_new();
    NB_CHECK(data != NULL);
    if (!data) {
        return;
    }
    NB_CHECK_INT(nb_file_read_all(path, data, 0, &err), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(data), expected);
    nb_buf_free(data);
}

static void test_stat(const char *base)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *file = nb_buf_new();
    nb_buf *missing = nb_buf_new();
    NB_CHECK(file != NULL && missing != NULL);
    if (!file || !missing) {
        nb_buf_free(file);
        nb_buf_free(missing);
        return;
    }

    NB_CHECK_INT(nb_path_join(file, base, "stat-file"), NB_OK);
    NB_CHECK_INT(nb_path_join(missing, base, "stat-missing"), NB_OK);
    NB_CHECK_INT(nb_file_write_all_atomic(nb_buf_cstr(file), "hello", 5, NULL,
                                          &err),
                 NB_OK);

    nb_file_info info;
    memset(&info, 0, sizeof(info));
    NB_CHECK_INT(nb_file_stat(nb_buf_cstr(file), true, &info, &err), NB_OK);
    NB_CHECK_INT(info.size, 5);
    NB_CHECK(info.is_regular);
    NB_CHECK(!info.is_dir);
    NB_CHECK(!info.is_symlink);
    NB_CHECK(info.mtime_ns != 0);
#ifndef _WIN32
    NB_CHECK_INT(info.mode & 0777, 0644);
#endif

    memset(&info, 0, sizeof(info));
    NB_CHECK_INT(nb_file_stat(nb_buf_cstr(file), false, &info, &err), NB_OK);
    NB_CHECK(info.is_regular);

    NB_CHECK_INT(nb_file_stat(base, true, &info, &err), NB_OK);
    NB_CHECK(info.is_dir);
    NB_CHECK(!info.is_regular);

    NB_CHECK_INT(nb_file_stat(nb_buf_cstr(missing), true, &info, &err),
                 NB_ERR_NOT_FOUND);

#ifndef _WIN32
    nb_buf *target = nb_buf_new();
    nb_buf *link = nb_buf_new();
    NB_CHECK(target != NULL && link != NULL);
    if (target && link) {
        NB_CHECK_INT(nb_path_join(target, base, "stat-target"), NB_OK);
        NB_CHECK_INT(nb_path_join(link, base, "stat-link"), NB_OK);
        NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(target), "abc", 3, &err),
                     NB_OK);
        NB_CHECK_INT(symlink("stat-target", nb_buf_cstr(link)), 0);

        memset(&info, 0, sizeof(info));
        NB_CHECK_INT(nb_file_stat(nb_buf_cstr(link), false, &info, &err),
                     NB_OK);
        NB_CHECK(info.is_symlink);
        NB_CHECK(!info.is_regular);

        memset(&info, 0, sizeof(info));
        NB_CHECK_INT(nb_file_stat(nb_buf_cstr(link), true, &info, &err),
                     NB_OK);
        NB_CHECK(info.is_regular);
        NB_CHECK_INT(info.size, 3);
    }
    nb_buf_free(link);
    nb_buf_free(target);
#endif

    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(file), &err), NB_OK);
    nb_buf_free(missing);
    nb_buf_free(file);
}

static void test_dir_entries(const char *base)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *dir_path = nb_buf_new();
    nb_buf *file_path = nb_buf_new();
    nb_buf *child_path = nb_buf_new();
    bool ready = dir_path && file_path && child_path;
    NB_CHECK(ready);
    if (!ready) {
        nb_buf_free(child_path);
        nb_buf_free(file_path);
        nb_buf_free(dir_path);
        return;
    }

    NB_CHECK_INT(nb_path_join(dir_path, base, "entries"), NB_OK);
    NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(dir_path), false, &err), NB_OK);
    NB_CHECK_INT(nb_path_join(file_path, nb_buf_cstr(dir_path), "file.txt"),
                 NB_OK);
    NB_CHECK_INT(nb_path_join(child_path, nb_buf_cstr(dir_path), "child"),
                 NB_OK);
    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(file_path), "x", 1, &err),
                 NB_OK);
    NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(child_path), false, &err), NB_OK);

    nb_dir *dir = nb_dir_open(nb_buf_cstr(dir_path), &err);
    NB_CHECK(dir != NULL);
    if (dir) {
        bool saw_file = false;
        bool saw_dir = false;
        int entries = 0;
        for (;;) {
            nb_dir_entry entry;
            memset(&entry, 0, sizeof(entry));
            const char *name = nb_dir_next(dir, &entry);
            if (!name) {
                break;
            }
            entries++;
            NB_CHECK(entry.name == name);
            if (strcmp(name, "file.txt") == 0) {
                NB_CHECK_INT(entry.type, NB_DIR_ENTRY_FILE);
                saw_file = true;
            } else if (strcmp(name, "child") == 0) {
                NB_CHECK_INT(entry.type, NB_DIR_ENTRY_DIR);
                saw_dir = true;
            } else {
                NB_CHECK(0);
            }
        }
        NB_CHECK_INT(entries, 2);
        NB_CHECK(saw_file);
        NB_CHECK(saw_dir);
        NB_CHECK_INT(nb_dir_status(dir), NB_OK);
        nb_dir_close(dir);
    }

    nb_dir *again = nb_dir_open(nb_buf_cstr(dir_path), &err);
    NB_CHECK(again != NULL);
    if (again) {
        NB_CHECK(nb_dir_next(again, NULL) != NULL);
        nb_dir_close(again);
    }

    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(file_path), &err), NB_OK);
    NB_CHECK_INT(nb_file_rmdir(nb_buf_cstr(child_path), &err), NB_OK);
    NB_CHECK_INT(nb_file_rmdir(nb_buf_cstr(dir_path), &err), NB_OK);
    nb_buf_free(child_path);
    nb_buf_free(file_path);
    nb_buf_free(dir_path);
}

static void test_temp(const char *base)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *path = nb_buf_new();
    NB_CHECK(path != NULL);
    if (!path) {
        return;
    }

    NB_CHECK_INT(nb_file_temp_in(path, base, NULL, &err), NB_OK);
    NB_CHECK(nb_file_exists(nb_buf_cstr(path)));
    NB_CHECK(nb_path_is_within(nb_buf_cstr(path), base, false));
    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(path), &err), NB_OK);

    int owned_fd = -1;
    NB_CHECK_INT(nb_file_temp_in(path, base, &owned_fd, &err), NB_OK);
    NB_CHECK(owned_fd >= 0);
    if (owned_fd >= 0) {
#ifdef _WIN32
        DWORD flags = HANDLE_FLAG_INHERIT;
        NB_CHECK(GetHandleInformation((HANDLE)_get_osfhandle(owned_fd), &flags));
        NB_CHECK((flags & HANDLE_FLAG_INHERIT) == 0);
#else
        NB_CHECK((fcntl(owned_fd, F_GETFD) & FD_CLOEXEC) != 0);
#endif
        NB_CHECK_INT(nb_fd_write_until(owned_fd, "a\0b", 3, INT64_MAX, NULL, NULL, &err), NB_OK);
        const char *prior_storage = path->ptr;
        NB_CHECK_INT(nb_file_temp_in(path, base, &owned_fd, &err), NB_ERR_STATE);
        NB_CHECK_INT(err.code, NB_ERR_STATE);
        NB_CHECK(path->ptr == prior_storage);
        char binary[3] = {0};
#ifdef _WIN32
        NB_CHECK_INT(_lseeki64(owned_fd, 0, SEEK_SET), 0);
        NB_CHECK_INT(_read(owned_fd, binary, sizeof(binary)), 3);
#else
        NB_CHECK_INT(lseek(owned_fd, 0, SEEK_SET), 0);
        NB_CHECK_INT(read(owned_fd, binary, sizeof(binary)), 3);
#endif
        NB_CHECK(memcmp(binary, "a\0b", sizeof(binary)) == 0);
        nb_buf contents = {0};
        NB_CHECK_INT(nb_file_read_all(nb_buf_cstr(path), &contents, 0, &err), NB_OK);
        NB_CHECK_INT(nb_buf_len(&contents), 3);
        NB_CHECK(nb_buf_len(&contents) == 3 && memcmp(nb_buf_cstr(&contents), "a\0b", 3) == 0);
        nb_buf_destroy(&contents);
#ifdef _WIN32
        NB_CHECK_INT(_close(owned_fd), 0);
#else
        NB_CHECK_INT(close(owned_fd), 0);
#endif
        NB_CHECK_INT(nb_file_remove(nb_buf_cstr(path), &err), NB_OK);
    }
    owned_fd = -1;
    const char *prior_storage = path->ptr;
    NB_CHECK_INT(nb_file_temp_in(path, "", &owned_fd, &err), NB_ERR_INVAL);
    NB_CHECK_INT(owned_fd, -1);
    NB_CHECK(path->ptr == prior_storage);
    nb_buf missing_parent = {0};
    NB_CHECK_INT(nb_path_join(&missing_parent, base, "absent-temp-parent"), NB_OK);
    NB_CHECK_INT(nb_file_temp_in(path, nb_buf_cstr(&missing_parent), &owned_fd, &err),
                 NB_ERR_NOT_FOUND);
    NB_CHECK_INT(err.code, NB_ERR_NOT_FOUND);
    NB_CHECK_INT(owned_fd, -1);
    NB_CHECK(path->ptr == prior_storage);
    NB_CHECK_INT(nb_file_temp_dir(path, nb_buf_cstr(&missing_parent), NULL, &err),
                 NB_ERR_NOT_FOUND);
    NB_CHECK_INT(err.code, NB_ERR_NOT_FOUND);
    NB_CHECK(path->ptr == prior_storage);
    nb_buf_destroy(&missing_parent);
    NB_CHECK_INT(nb_file_temp_dir(path, base, "../escape-", &err), NB_ERR_INVAL);
    NB_CHECK(path->ptr == prior_storage);
    NB_CHECK_INT(nb_file_temp_dir(path, base, ".private-", &err), NB_OK);
    NB_CHECK(nb_path_is_within(nb_buf_cstr(path), base, false));
    NB_CHECK(strncmp(nb_path_basename(nb_buf_cstr(path)), ".private-", 9) == 0);
#ifndef _WIN32
    nb_file_info private_directory = {0};
    NB_CHECK_INT(nb_file_stat(nb_buf_cstr(path), false, &private_directory, &err), NB_OK);
    NB_CHECK_INT(private_directory.mode & 0777, 0700);
#endif
    NB_CHECK_INT(nb_file_rmdir(nb_buf_cstr(path), &err), NB_OK);

    NB_CHECK_INT(nb_file_temp_in(path, NULL, NULL, &err), NB_ERR_INVAL);
    NB_CHECK_INT(nb_file_temp_in(NULL, base, NULL, &err), NB_ERR_INVAL);

    NB_CHECK_INT(nb_file_temp_dir(path, NULL, NULL, &err), NB_OK);
    NB_CHECK(nb_file_is_dir(nb_buf_cstr(path)));
    NB_CHECK_INT(nb_file_rmdir(nb_buf_cstr(path), &err), NB_OK);

    NB_CHECK_INT(nb_file_temp(path, &err), NB_OK);
    NB_CHECK(nb_file_exists(nb_buf_cstr(path)));
    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(path), &err), NB_OK);

    nb_buf_free(path);
}

static void test_sync_dir(const char *base)
{
    nb_err err;
    nb_err_clear(&err);
    NB_CHECK_INT(nb_file_sync_dir(base, &err), NB_OK);

    nb_buf *missing = nb_buf_new();
    NB_CHECK(missing != NULL);
    if (missing) {
        NB_CHECK_INT(nb_path_join(missing, base, "no-such-dir"), NB_OK);
        NB_CHECK_INT(nb_file_sync_dir(nb_buf_cstr(missing), &err),
                     NB_ERR_NOT_FOUND);
        nb_buf_free(missing);
    }

    nb_buf *file = nb_buf_new();
    NB_CHECK(file != NULL);
    if (file) {
        NB_CHECK_INT(nb_path_join(file, base, "durable.txt"), NB_OK);
        nb_file_write_options options = { 0 };
        options.mode = 0640;
        options.sync_dir = true;
        options.follow_symlink = true;
        NB_CHECK_INT(nb_file_write_all_atomic(
                         nb_buf_cstr(file), "durable", 7, &options, &err),
                     NB_OK);
        check_contents(nb_buf_cstr(file), "durable");
#ifndef _WIN32
        struct stat info;
        NB_CHECK_INT(stat(nb_buf_cstr(file), &info), 0);
        NB_CHECK_INT((long long)(info.st_mode & 0777), 0640);
#endif
        NB_CHECK_INT(nb_file_remove(nb_buf_cstr(file), &err), NB_OK);
        nb_buf_free(file);
    }
}

static void test_canonical(const char *base)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *canonical_base = nb_buf_new();
    nb_buf *dot = nb_buf_new();
    nb_buf *canonical_dot = nb_buf_new();
    bool ready = canonical_base && dot && canonical_dot;
    NB_CHECK(ready);
    if (!ready) {
        nb_buf_free(canonical_dot);
        nb_buf_free(dot);
        nb_buf_free(canonical_base);
        return;
    }

    NB_CHECK_INT(nb_path_canonical(canonical_base, base, &err), NB_OK);
    NB_CHECK(nb_path_is_absolute(nb_buf_cstr(canonical_base)));
    NB_CHECK_INT(nb_path_join(dot, base, "."), NB_OK);
    NB_CHECK_INT(nb_path_canonical(canonical_dot, nb_buf_cstr(dot), &err),
                 NB_OK);
    NB_CHECK_STR(nb_buf_cstr(canonical_dot), nb_buf_cstr(canonical_base));

    nb_buf_clear(canonical_dot);
    NB_CHECK(nb_buf_append_cstr(canonical_dot, "stale"));
    NB_CHECK_INT(nb_path_canonical(canonical_dot,
                                   "/definitely-not-a-real-nbase-path", &err),
                 NB_ERR_NOT_FOUND);
    NB_CHECK_STR(nb_buf_cstr(canonical_dot), "stale");

    nb_buf *missing = nb_buf_new();
    nb_buf *missing_tail = nb_buf_new();
    nb_buf *missing_expected = nb_buf_new();
    bool missing_ready = missing && missing_tail && missing_expected;
    NB_CHECK(missing_ready);
    if (missing_ready) {
        NB_CHECK_INT(nb_path_canonical_missing(missing, base, &err), NB_OK);
        NB_CHECK_STR(nb_buf_cstr(missing), nb_buf_cstr(canonical_base));

        NB_CHECK_INT(nb_path_join(missing_tail, base, "new/dir/file"), NB_OK);
        NB_CHECK_INT(nb_path_canonical_missing(missing, nb_buf_cstr(missing_tail), &err), NB_OK);
        NB_CHECK_INT(nb_path_join(missing_expected, nb_buf_cstr(canonical_base), "new/dir/file"),
                     NB_OK);
        NB_CHECK_STR(nb_buf_cstr(missing), nb_buf_cstr(missing_expected));

        NB_CHECK_INT(nb_path_join(missing_tail, base, "new/../escape"), NB_OK);
        NB_CHECK_INT(nb_path_canonical_missing(missing, nb_buf_cstr(missing_tail), &err),
                     NB_ERR_INVAL);
    }
    nb_buf_free(missing_expected);
    nb_buf_free(missing_tail);
    nb_buf_free(missing);

    nb_buf *sub = nb_buf_new();
    nb_buf *sub2 = nb_buf_new();
    NB_CHECK(sub != NULL && sub2 != NULL);
    if (sub && sub2) {
        NB_CHECK_INT(nb_path_join(sub, base, "sub"), NB_OK);
        NB_CHECK_INT(nb_path_join(sub2, base, "sub2"), NB_OK);
        NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(sub), false, &err), NB_OK);
        NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(sub2), false, &err), NB_OK);

        NB_CHECK(nb_path_is_within(nb_buf_cstr(sub), base, false));
        NB_CHECK(nb_path_is_within(base, base, false));
        NB_CHECK(!nb_path_is_within(base, nb_buf_cstr(sub), false));
        /* Prefix at a component boundary, not a string prefix. */
        NB_CHECK(!nb_path_is_within(nb_buf_cstr(sub2), nb_buf_cstr(sub), false));
        NB_CHECK(!nb_path_is_within(NULL, base, false));
        NB_CHECK(!nb_path_is_within(base, NULL, false));

        NB_CHECK_INT(nb_file_rmdir(nb_buf_cstr(sub2), &err), NB_OK);
        NB_CHECK_INT(nb_file_rmdir(nb_buf_cstr(sub), &err), NB_OK);
    }
    nb_buf_free(sub2);
    nb_buf_free(sub);
    nb_buf_free(canonical_dot);
    nb_buf_free(dot);
    nb_buf_free(canonical_base);
}

#ifndef _WIN32
/* The POSIX canonicalizer used to write into a fixed 256-byte allocation, so
 * an existing path longer than that overflowed before any growth check. */
static void test_canonical_long_path(const char *base)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *path = nb_buf_new();
    nb_buf *canonical = nb_buf_new();
    nb_buf *suffix = nb_buf_new();
    bool ready = path != NULL && canonical != NULL && suffix != NULL;
    NB_CHECK(ready);
    if (!ready) {
        nb_buf_free(suffix);
        nb_buf_free(canonical);
        nb_buf_free(path);
        return;
    }

    char component[101];
    bool built = nb_buf_append_cstr(path, base);
    for (size_t i = 0; built && i < 3u; i++) {
        memset(component, (int)('a' + (char)i), 100u);
        component[100] = '\0';
        built = nb_buf_append_char(path, '/') && nb_buf_append_cstr(path, component) &&
                nb_buf_append_char(suffix, '/') && nb_buf_append_cstr(suffix, component);
    }
    NB_CHECK(built);
    if (built) {
        NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(path), true, &err), NB_OK);
        NB_CHECK_INT(nb_path_canonical(canonical, nb_buf_cstr(path), &err), NB_OK);
        NB_CHECK(nb_buf_len(canonical) > 256u);
        NB_CHECK(nb_path_is_absolute(nb_buf_cstr(canonical)));
        if (nb_buf_len(canonical) >= nb_buf_len(suffix)) {
            const char *tail = nb_buf_cstr(canonical) + nb_buf_len(canonical) - nb_buf_len(suffix);
            NB_CHECK_MEM(tail, nb_buf_cstr(suffix), nb_buf_len(suffix));
        } else {
            NB_CHECK(0);
        }
    }

    nb_buf_free(suffix);
    nb_buf_free(canonical);
    nb_buf_free(path);
}
#endif

static void test_locking(const char *base)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *path = nb_buf_new();
    NB_CHECK(path != NULL);
    if (!path) {
        return;
    }
    NB_CHECK_INT(nb_path_join(path, base, "lockfile"), NB_OK);

    nb_file_lock *first = NULL;
    nb_file_lock *second = NULL;
    NB_CHECK_INT(nb_file_lock_acquire(nb_buf_cstr(path), true, false, &first,
                                      &err),
                 NB_OK);
    NB_CHECK(first != NULL);
    NB_CHECK_INT(nb_file_lock_acquire(nb_buf_cstr(path), true, false, &second,
                                      &err),
                 NB_ERR_AGAIN);
    NB_CHECK(second == NULL);
    nb_file_lock_release(first);

    NB_CHECK_INT(nb_file_lock_acquire(nb_buf_cstr(path), true, false, &first,
                                      &err),
                 NB_OK);
    nb_file_lock_release(first);
    NB_CHECK(nb_file_exists(nb_buf_cstr(path)));

    nb_file_lock *shared_one = NULL;
    nb_file_lock *shared_two = NULL;
    NB_CHECK_INT(nb_file_lock_acquire(nb_buf_cstr(path), false, false,
                                      &shared_one, &err),
                 NB_OK);
    NB_CHECK_INT(nb_file_lock_acquire(nb_buf_cstr(path), false, false,
                                      &shared_two, &err),
                 NB_OK);
    nb_file_lock_release(shared_two);
    nb_file_lock_release(shared_one);

    NB_CHECK_INT(nb_file_lock_acquire(NULL, true, false, &first, &err),
                 NB_ERR_INVAL);
    nb_file_lock_release(NULL);
    nb_buf_free(path);
}

static void test_atomic_options(const char *base)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *file = nb_buf_new();
    NB_CHECK(file != NULL);
    if (!file) {
        return;
    }
    NB_CHECK_INT(nb_path_join(file, base, "atomic.txt"), NB_OK);

    nb_file_write_options options = { 0 };
    options.create_only = true;
    options.sync_dir = true;
    options.follow_symlink = true;
    bool published = false;
    options.published = &published;
    NB_CHECK_INT(nb_file_write_all_atomic(nb_buf_cstr(file), "one", 3,
                                          &options, &err),
                 NB_OK);
    NB_CHECK(published);
    NB_CHECK_INT(nb_file_write_all_atomic(nb_buf_cstr(file), "two", 3,
                                          &options, &err),
                 NB_ERR_EXISTS);
    NB_CHECK(!published);
    check_contents(nb_buf_cstr(file), "one");

    nb_file_write_options mode_options = { 0 };
    mode_options.mode = 0600;
    mode_options.follow_symlink = true;
    NB_CHECK_INT(nb_file_write_all_atomic(nb_buf_cstr(file), "mode", 4,
                                          &mode_options, &err),
                 NB_OK);
#ifndef _WIN32
    struct stat info;
    NB_CHECK_INT(stat(nb_buf_cstr(file), &info), 0);
    NB_CHECK_INT((long long)(info.st_mode & 0777), 0600);
    NB_CHECK_INT(chmod(nb_buf_cstr(file), 0640), 0);
    nb_file_write_options preserve = { 0 };
    preserve.follow_symlink = true;
    NB_CHECK_INT(nb_file_write_all_atomic(nb_buf_cstr(file), "keep", 4,
                                          &preserve, &err),
                 NB_OK);
    NB_CHECK_INT(stat(nb_buf_cstr(file), &info), 0);
    NB_CHECK_INT((long long)(info.st_mode & 0777), 0640);

    nb_buf *fresh = nb_buf_new();
    NB_CHECK(fresh != NULL);
    if (fresh) {
        NB_CHECK_INT(nb_path_join(fresh, base, "atomic-new.txt"), NB_OK);
        nb_file_write_options private_options = {0};
        private_options.new_file_mode = 0600;
        private_options.follow_symlink = true;
        bool fresh_published = false;
        private_options.published = &fresh_published;
        NB_CHECK_INT(
            nb_file_write_all_atomic(nb_buf_cstr(fresh), "secret", 6, &private_options, &err),
            NB_OK);
        NB_CHECK(fresh_published);
        struct stat fresh_info;
        NB_CHECK_INT(stat(nb_buf_cstr(fresh), &fresh_info), 0);
        NB_CHECK_INT((long long)(fresh_info.st_mode & 0777), 0600);
        NB_CHECK_INT(nb_file_remove(nb_buf_cstr(fresh), &err), NB_OK);
        nb_buf_free(fresh);
    }
#endif

    nb_buf *target = nb_buf_new();
    nb_buf *link = nb_buf_new();
    NB_CHECK(target != NULL && link != NULL);
    if (target && link) {
#ifndef _WIN32
        NB_CHECK_INT(nb_path_join(target, base, "atomic-target"), NB_OK);
        NB_CHECK_INT(nb_path_join(link, base, "atomic-link"), NB_OK);
        NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(target), "target", 6,
                                       &err),
                     NB_OK);
        NB_CHECK_INT(symlink("atomic-target", nb_buf_cstr(link)), 0);

        /* NULL options: mode 0, create_only false, sync_dir true,
         * follow_symlink false. */
        NB_CHECK_INT(nb_file_write_all_atomic(nb_buf_cstr(link), "payload", 7,
                                              NULL, &err),
                     NB_ERR_IO);
        check_contents(nb_buf_cstr(link), "target");
        nb_file_info link_info;
        memset(&link_info, 0, sizeof(link_info));
        NB_CHECK_INT(nb_file_stat(nb_buf_cstr(link), false, &link_info, &err),
                     NB_OK);
        NB_CHECK(link_info.is_symlink);

        nb_file_write_options follow = { 0 };
        follow.follow_symlink = true;
        NB_CHECK_INT(nb_file_write_all_atomic(nb_buf_cstr(link), "payload", 7,
                                              &follow, &err),
                     NB_OK);
        memset(&link_info, 0, sizeof(link_info));
        NB_CHECK_INT(nb_file_stat(nb_buf_cstr(link), false, &link_info, &err),
                     NB_OK);
        NB_CHECK(link_info.is_regular);
        check_contents(nb_buf_cstr(link), "payload");
        check_contents(nb_buf_cstr(target), "target");
#endif
        nb_buf_free(link);
        nb_buf_free(target);
    }

    nb_buf *dir_path = nb_buf_new();
    NB_CHECK(dir_path != NULL);
    if (dir_path) {
        NB_CHECK_INT(nb_path_join(dir_path, base, "atomic-dir"), NB_OK);
        NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(dir_path), false, &err), NB_OK);
        NB_CHECK_INT(nb_file_write_all_atomic(nb_buf_cstr(dir_path), "x", 1,
                                              NULL, &err),
                     NB_ERR_IO);
        NB_CHECK(nb_file_is_dir(nb_buf_cstr(dir_path)));
        NB_CHECK_INT(nb_file_rmdir(nb_buf_cstr(dir_path), &err), NB_OK);
        nb_buf_free(dir_path);
    }

    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(file), &err), NB_OK);
    nb_buf_free(file);
}

static void test_rename_noreplace(const char *base)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *from = nb_buf_new();
    nb_buf *to = nb_buf_new();
    nb_buf *other = nb_buf_new();
    bool ready = from && to && other;
    NB_CHECK(ready);
    if (!ready) {
        nb_buf_free(other);
        nb_buf_free(to);
        nb_buf_free(from);
        return;
    }

    NB_CHECK_INT(nb_path_join(from, base, "rename-from"), NB_OK);
    NB_CHECK_INT(nb_path_join(to, base, "rename-to"), NB_OK);
    NB_CHECK_INT(nb_path_join(other, base, "rename-other"), NB_OK);
    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(from), "A", 1, &err), NB_OK);
    NB_CHECK_INT(nb_file_rename_noreplace(nb_buf_cstr(from), nb_buf_cstr(to),
                                          &err),
                 NB_OK);
    NB_CHECK(!nb_file_exists(nb_buf_cstr(from)));
    check_contents(nb_buf_cstr(to), "A");

    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(other), "B", 1, &err), NB_OK);
    NB_CHECK_INT(nb_file_rename_noreplace(nb_buf_cstr(other), nb_buf_cstr(to),
                                          &err),
                 NB_ERR_EXISTS);
    NB_CHECK(nb_file_exists(nb_buf_cstr(other)));
    check_contents(nb_buf_cstr(to), "A");

    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(other), &err), NB_OK);
    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(to), &err), NB_OK);
    nb_buf_free(other);
    nb_buf_free(to);
    nb_buf_free(from);
}

static void test_set_mode(const char *base)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *file = nb_buf_new();
    nb_buf *missing = nb_buf_new();
    NB_CHECK(file != NULL && missing != NULL);
    if (!file || !missing) {
        nb_buf_free(missing);
        nb_buf_free(file);
        return;
    }
    NB_CHECK_INT(nb_path_join(file, base, "mode-file"), NB_OK);
    NB_CHECK_INT(nb_path_join(missing, base, "mode-missing"), NB_OK);
    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(file), "mode", 4, &err),
                 NB_OK);

    NB_CHECK_INT(nb_file_set_mode(nb_buf_cstr(file), 0444, &err), NB_OK);
    nb_file_info info;
    memset(&info, 0, sizeof(info));
    NB_CHECK_INT(nb_file_stat(nb_buf_cstr(file), true, &info, &err), NB_OK);
    NB_CHECK_INT(info.mode & 0777, 0444);

    NB_CHECK_INT(nb_file_set_mode(nb_buf_cstr(file), 0644, &err), NB_OK);
    memset(&info, 0, sizeof(info));
    NB_CHECK_INT(nb_file_stat(nb_buf_cstr(file), true, &info, &err), NB_OK);
    NB_CHECK_INT(info.mode & 0777, 0644);

    NB_CHECK_INT(nb_file_set_mode(nb_buf_cstr(missing), 0600, &err),
                 NB_ERR_NOT_FOUND);
    NB_CHECK_INT(nb_file_set_mode(NULL, 0600, &err), NB_ERR_INVAL);

    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(file), &err), NB_OK);
    nb_buf_free(missing);
    nb_buf_free(file);
}

static void test_copy_file(const char *base)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *src = nb_buf_new();
    nb_buf *dst = nb_buf_new();
    nb_buf *dir_dst = nb_buf_new();
    bool ready = src && dst && dir_dst;
    NB_CHECK(ready);
    if (!ready) {
        nb_buf_free(dir_dst);
        nb_buf_free(dst);
        nb_buf_free(src);
        return;
    }
    NB_CHECK_INT(nb_path_join(src, base, "copy-src"), NB_OK);
    NB_CHECK_INT(nb_path_join(dst, base, "copy-dst"), NB_OK);
    NB_CHECK_INT(nb_path_join(dir_dst, base, "copy-dst-dir"), NB_OK);

    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(src), "one", 3, &err), NB_OK);
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(src), nb_buf_cstr(dst), NULL, &err),
                 NB_OK);
    check_contents(nb_buf_cstr(dst), "one");

    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(src), "two", 3, &err), NB_OK);
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(src), nb_buf_cstr(dst), NULL, &err),
                 NB_ERR_EXISTS);
    check_contents(nb_buf_cstr(dst), "one");

    nb_file_copy_options overwrite = { 0 };
    overwrite.overwrite = true;
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(src), nb_buf_cstr(dst), &overwrite,
                              &err),
                 NB_OK);
    check_contents(nb_buf_cstr(dst), "two");

    NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(dir_dst), false, &err), NB_OK);
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(src), nb_buf_cstr(dir_dst),
                              &overwrite, &err),
                 NB_ERR_EXISTS);
    NB_CHECK(nb_file_is_dir(nb_buf_cstr(dir_dst)));
    NB_CHECK_INT(nb_file_rmdir(nb_buf_cstr(dir_dst), &err), NB_OK);

#ifndef _WIN32
    NB_CHECK_INT(nb_file_set_mode(nb_buf_cstr(src), 0600, &err), NB_OK);
    nb_file_copy_options preserve = { 0 };
    preserve.overwrite = true;
    preserve.preserve_mode = true;
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(src), nb_buf_cstr(dst), &preserve,
                              &err),
                 NB_OK);
    struct stat info;
    NB_CHECK_INT(stat(nb_buf_cstr(dst), &info), 0);
    NB_CHECK_INT((long long)(info.st_mode & 0777), 0600);
#endif

    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(dst), &err), NB_OK);
    nb_file_copy_options private_copy = {0};
    private_copy.new_file_mode = 0600;
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(src), nb_buf_cstr(dst), &private_copy, &err), NB_OK);
    nb_file_info copied;
    NB_CHECK_INT(nb_file_stat(nb_buf_cstr(dst), false, &copied, &err), NB_OK);
#ifdef _WIN32
    NB_CHECK_INT(copied.mode & 0777u, 0644);
#else
    NB_CHECK_INT(copied.mode & 0777u, 0600);
#endif
    NB_CHECK_INT(nb_file_set_mode(nb_buf_cstr(dst), 0640, &err), NB_OK);
    private_copy.overwrite = true;
    private_copy.new_file_mode = 0400;
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(src), nb_buf_cstr(dst), &private_copy, &err), NB_OK);
    NB_CHECK_INT(nb_file_stat(nb_buf_cstr(dst), false, &copied, &err), NB_OK);
#ifdef _WIN32
    NB_CHECK_INT(copied.mode & 0777u, 0644);
#else
    NB_CHECK_INT(copied.mode & 0777u, 0640);
#endif
    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(dst), &err), NB_OK);
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(src), nb_buf_cstr(dst), &private_copy, &err), NB_OK);
    NB_CHECK_INT(nb_file_stat(nb_buf_cstr(dst), false, &copied, &err), NB_OK);
#ifdef _WIN32
    NB_CHECK_INT(copied.mode & 0777u, 0444);
#else
    NB_CHECK_INT(copied.mode & 0777u, 0400);
#endif
    NB_CHECK_INT(nb_file_set_mode(nb_buf_cstr(dst), 0600, &err), NB_OK);
    private_copy.preserve_mode = true;
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(src), nb_buf_cstr(dst), &private_copy, &err), NB_OK);
    nb_file_info source_mode;
    NB_CHECK_INT(nb_file_stat(nb_buf_cstr(src), false, &source_mode, &err), NB_OK);
    NB_CHECK_INT(nb_file_stat(nb_buf_cstr(dst), false, &copied, &err), NB_OK);
    NB_CHECK_INT(copied.mode & 0777u, source_mode.mode & 0777u);

    NB_CHECK_INT(nb_file_copy(NULL, nb_buf_cstr(dst), NULL, &err),
                 NB_ERR_INVAL);
    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(src), &err), NB_OK);
    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(dst), &err), NB_OK);
    nb_buf_free(dir_dst);
    nb_buf_free(dst);
    nb_buf_free(src);
}

static void test_copy_large_file(const char *base)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *src = nb_buf_new();
    nb_buf *dst = nb_buf_new();
    nb_buf *data = nb_buf_new();
    nb_buf *copied = nb_buf_new();
    bool ready = src && dst && data && copied;
    NB_CHECK(ready);
    if (!ready) {
        nb_buf_free(copied);
        nb_buf_free(data);
        nb_buf_free(dst);
        nb_buf_free(src);
        return;
    }

    size_t const total = 1024u * 1024u + 123u;
    unsigned char chunk[4096];
    size_t produced = 0;
    bool built = true;
    while (built && produced < total) {
        size_t amount = total - produced;
        if (amount > sizeof(chunk)) {
            amount = sizeof(chunk);
        }
        for (size_t i = 0; i < amount; i++) {
            size_t index = produced + i;
            chunk[i] = (unsigned char)(index * 31u + index / 251u);
        }
        built = nb_buf_append(data, chunk, amount);
        produced += amount;
    }
    NB_CHECK(built);
    if (built) {
        NB_CHECK_INT(nb_path_join(src, base, "copy-large-src"), NB_OK);
        NB_CHECK_INT(nb_path_join(dst, base, "copy-large-dst"), NB_OK);
        NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(src), nb_buf_cstr(data), nb_buf_len(data), &err),
                     NB_OK);
        NB_CHECK_INT(nb_file_copy(nb_buf_cstr(src), nb_buf_cstr(dst), NULL, &err), NB_OK);
        NB_CHECK_INT(nb_file_read_all(nb_buf_cstr(dst), copied, 0, &err), NB_OK);
        NB_CHECK_INT(nb_buf_len(copied), nb_buf_len(data));
        if (nb_buf_len(copied) == nb_buf_len(data)) {
            NB_CHECK_MEM(nb_buf_cstr(copied), nb_buf_cstr(data), nb_buf_len(data));
        }
        NB_CHECK_INT(nb_file_remove(nb_buf_cstr(src), &err), NB_OK);
        NB_CHECK_INT(nb_file_remove(nb_buf_cstr(dst), &err), NB_OK);
    }

    nb_buf_free(copied);
    nb_buf_free(data);
    nb_buf_free(dst);
    nb_buf_free(src);
}

static void test_copy_directory(const char *base)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *src = nb_buf_new();
    nb_buf *dst = nb_buf_new();
    nb_buf *sub = nb_buf_new();
    nb_buf *nested = nb_buf_new();
    nb_buf *dst_nested = nb_buf_new();
    nb_buf *src_top = nb_buf_new();
    nb_buf *dst_top = nb_buf_new();
    bool ready =
        src && dst && sub && nested && dst_nested && src_top && dst_top;
    NB_CHECK(ready);
    if (!ready) {
        nb_buf_free(dst_top);
        nb_buf_free(src_top);
        nb_buf_free(dst_nested);
        nb_buf_free(nested);
        nb_buf_free(sub);
        nb_buf_free(dst);
        nb_buf_free(src);
        return;
    }
    NB_CHECK_INT(nb_path_join(src, base, "copy-dir-src"), NB_OK);
    NB_CHECK_INT(nb_path_join(dst, base, "copy-dir-dst"), NB_OK);
    NB_CHECK_INT(nb_path_join(sub, nb_buf_cstr(src), "sub"), NB_OK);
    NB_CHECK_INT(nb_path_join(nested, nb_buf_cstr(sub), "nested.txt"), NB_OK);
    NB_CHECK_INT(nb_path_join(dst_nested, nb_buf_cstr(dst),
                              "sub/nested.txt"),
                 NB_OK);
    NB_CHECK_INT(nb_path_join(src_top, nb_buf_cstr(src), "top.txt"), NB_OK);
    NB_CHECK_INT(nb_path_join(dst_top, nb_buf_cstr(dst), "top.txt"), NB_OK);

    NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(src), false, &err), NB_OK);
    NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(sub), false, &err), NB_OK);
    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(nested), "nested", 6, &err),
                 NB_OK);
    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(src_top), "top", 3, &err),
                 NB_OK);

    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(src), nb_buf_cstr(dst), NULL, &err),
                 NB_ERR_UNSUPPORTED);
    NB_CHECK(!nb_file_exists(nb_buf_cstr(dst)));

    nb_file_copy_options recursive = { 0 };
    recursive.recursive = true;
    recursive.overwrite = true;
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(src), nb_buf_cstr(dst), &recursive,
                              &err),
                 NB_OK);
    check_contents(nb_buf_cstr(dst_nested), "nested");
    check_contents(nb_buf_cstr(dst_top), "top");

    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(src_top), "newer", 5, &err),
                 NB_OK);
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(src), nb_buf_cstr(dst), &recursive,
                              &err),
                 NB_OK);
    check_contents(nb_buf_cstr(dst_top), "newer");

    /* A directory copy may replace an existing file when overwrite is set. */
    nb_buf *existing_file = nb_buf_new();
    NB_CHECK(existing_file != NULL);
    if (existing_file) {
        NB_CHECK_INT(nb_path_join(existing_file, base, "copy-dir-file"),
                     NB_OK);
        NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(existing_file), "x", 1,
                                       &err),
                     NB_OK);
        NB_CHECK_INT(nb_file_copy(nb_buf_cstr(src),
                                  nb_buf_cstr(existing_file), &recursive,
                                  &err),
                     NB_OK);
        NB_CHECK(nb_file_is_dir(nb_buf_cstr(existing_file)));
        NB_CHECK_INT(nb_file_remove_all(nb_buf_cstr(existing_file), 0, &err), NB_OK);
        nb_buf_free(existing_file);
    }

    NB_CHECK_INT(nb_file_remove_all(nb_buf_cstr(src), 0, &err), NB_OK);
    NB_CHECK_INT(nb_file_remove_all(nb_buf_cstr(dst), 0, &err), NB_OK);
    nb_buf_free(dst_top);
    nb_buf_free(src_top);
    nb_buf_free(dst_nested);
    nb_buf_free(nested);
    nb_buf_free(sub);
    nb_buf_free(dst);
    nb_buf_free(src);
}

static void test_symlink_readlink(const char *base)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *target = nb_buf_new();
    nb_buf *link = nb_buf_new();
    nb_buf *missing = nb_buf_new();
    nb_buf *read = nb_buf_new();
    bool ready = target && link && missing && read;
    NB_CHECK(ready);
    if (!ready) {
        nb_buf_free(read);
        nb_buf_free(missing);
        nb_buf_free(link);
        nb_buf_free(target);
        return;
    }
    NB_CHECK_INT(nb_path_join(target, base, "sym-target"), NB_OK);
    NB_CHECK_INT(nb_path_join(link, base, "sym-link"), NB_OK);
    NB_CHECK_INT(nb_path_join(missing, base, "sym-missing"), NB_OK);
    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(target), "target", 6, &err),
                 NB_OK);

    nb_status status = nb_file_symlink("sym-target", nb_buf_cstr(link), &err);
    if (status != NB_ERR_UNSUPPORTED) {
        NB_CHECK_INT(status, NB_OK);
        if (status == NB_OK) {
            NB_CHECK_INT(nb_file_readlink(read, nb_buf_cstr(link), &err),
                         NB_OK);
            NB_CHECK_STR(nb_buf_cstr(read), "sym-target");
            NB_CHECK_INT(nb_file_symlink("sym-target", nb_buf_cstr(link),
                                         &err),
                         NB_ERR_EXISTS);

            nb_buf_clear(read);
            NB_CHECK(nb_buf_append_cstr(read, "stale"));
            NB_CHECK_INT(nb_file_readlink(read, nb_buf_cstr(target), &err),
                         NB_ERR_INVAL);
            NB_CHECK_STR(nb_buf_cstr(read), "stale");
        }
    }
    NB_CHECK_INT(nb_file_readlink(read, nb_buf_cstr(missing), &err),
                 NB_ERR_NOT_FOUND);
    NB_CHECK_INT(nb_file_readlink(NULL, nb_buf_cstr(link), &err),
                 NB_ERR_INVAL);

    nb_buf_free(read);
    nb_buf_free(missing);
    nb_buf_free(link);
    nb_buf_free(target);
}

struct copy_filter_test_context {
    const char *skip;
    const char *reject;
    bool mutate_link;
    bool replace_regular;
    size_t calls;
};

static nb_status copy_filter_test(const char *source, const char *destination,
                                  const nb_file_copy_entry *entry, bool *include, void *context,
                                  nb_err *err)
{
    struct copy_filter_test_context *test = context;
    const char *name = nb_path_basename(source);
    NB_CHECK(destination != NULL);
    NB_CHECK(*include);
    test->calls++;
    if (test->skip && !strcmp(name, test->skip))
        *include = false;
    if (test->reject && !strcmp(name, test->reject)) {
        nb_err_set(err, NB_ERR_PERMISSION, "copy rejected by test policy");
        return NB_ERR_PERMISSION;
    }
    if (test->mutate_link) {
        NB_CHECK(entry->info.is_symlink);
        NB_CHECK_STR(entry->symlink_target, "original-target");
        nb_status status = nb_file_remove(source, err);
        if (status == NB_OK)
            status = nb_file_symlink("replacement-target", source, err);
        return status;
    }
    if (test->replace_regular) {
        NB_CHECK(entry->info.is_regular);
        nb_status status = nb_file_remove(source, err);
        if (status == NB_OK)
            status = nb_file_symlink("replacement-target", source, err);
        return status;
    }
    return NB_OK;
}

static void test_copy_policy(const char *base)
{
    nb_err err = {0};
    nb_buf source = {0};
    nb_buf target = {0};
    nb_buf child = {0};
    nb_buf content = {0};
    NB_CHECK_INT(nb_path_join(&source, base, "copy-policy-source"), NB_OK);
    NB_CHECK_INT(nb_path_join(&target, base, "copy-policy-target"), NB_OK);
    NB_CHECK_INT(nb_path_join(&child, nb_buf_cstr(&source), "skip/nested"), NB_OK);
    NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(&child), true, &err), NB_OK);
    NB_CHECK_INT(nb_path_join(&child, nb_buf_cstr(&child), "deep.txt"), NB_OK);
    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(&child), "deep", 4, &err), NB_OK);
    NB_CHECK_INT(nb_path_join(&child, nb_buf_cstr(&source), "keep.txt"), NB_OK);
    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(&child), "keep", 4, &err), NB_OK);
    struct copy_filter_test_context policy = {.skip = "skip"};
    nb_file_copy_options options = {.recursive = true,
                                    .directory_mode = 0700,
                                    .max_depth = 1,
                                    .filter = copy_filter_test,
                                    .filter_context = &policy};
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(&source), nb_buf_cstr(&target), &options, &err), NB_OK);
    NB_CHECK(policy.calls >= 3);
    NB_CHECK_INT(nb_path_join(&child, nb_buf_cstr(&target), "skip"), NB_OK);
    NB_CHECK(!nb_file_exists(nb_buf_cstr(&child)));
    NB_CHECK_INT(nb_path_join(&child, nb_buf_cstr(&target), "keep.txt"), NB_OK);
    check_contents(nb_buf_cstr(&child), "keep");
#ifndef _WIN32
    nb_file_info info;
    NB_CHECK_INT(nb_file_stat(nb_buf_cstr(&target), false, &info, &err), NB_OK);
    NB_CHECK_INT(info.mode & 0777u, 0700);
#endif
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(&source), nb_buf_cstr(&target), &options, &err),
                 NB_ERR_EXISTS);
    NB_CHECK_INT(nb_file_remove_all(nb_buf_cstr(&target), 0, &err), NB_OK);
    policy.skip = NULL;
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(&source), nb_buf_cstr(&target), &options, &err),
                 NB_ERR_LOOP);
    NB_CHECK(!nb_file_exists(nb_buf_cstr(&target)));
    options.max_depth = 0;
    policy.reject = "keep.txt";
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(&source), nb_buf_cstr(&target), &options, &err),
                 NB_ERR_PERMISSION);
    NB_CHECK(!nb_file_exists(nb_buf_cstr(&target)));
    NB_CHECK_INT(nb_file_remove_all(nb_buf_cstr(&source), 0, &err), NB_OK);

    NB_CHECK_INT(nb_path_join(&source, base, "copy-snapshot-link"), NB_OK);
    NB_CHECK_INT(nb_path_join(&target, base, "copy-snapshot-result"), NB_OK);
    nb_status linked = nb_file_symlink("original-target", nb_buf_cstr(&source), &err);
    if (linked != NB_ERR_UNSUPPORTED) {
        NB_CHECK_INT(linked, NB_OK);
        if (linked == NB_OK) {
            policy = (struct copy_filter_test_context){.mutate_link = true};
            options.copy_symlinks = true;
            NB_CHECK_INT(nb_file_copy(nb_buf_cstr(&source), nb_buf_cstr(&target), &options, &err),
                         NB_OK);
            NB_CHECK_INT(nb_file_readlink(&content, nb_buf_cstr(&target), &err), NB_OK);
            NB_CHECK_STR(nb_buf_cstr(&content), "original-target");
            NB_CHECK_INT(nb_file_remove(nb_buf_cstr(&target), &err), NB_OK);
            NB_CHECK_INT(nb_file_remove(nb_buf_cstr(&source), &err), NB_OK);
            NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(&source), "regular", 7, &err), NB_OK);
            policy = (struct copy_filter_test_context){.replace_regular = true};
            NB_CHECK(nb_file_copy(nb_buf_cstr(&source), nb_buf_cstr(&target), &options, &err) !=
                     NB_OK);
            NB_CHECK(!nb_file_exists(nb_buf_cstr(&target)));
            NB_CHECK_INT(nb_file_remove(nb_buf_cstr(&source), &err), NB_OK);
        }
    }
    nb_buf_destroy(&content);
    nb_buf_destroy(&child);
    nb_buf_destroy(&target);
    nb_buf_destroy(&source);
}

static void test_copy_symlinks(const char *base)
{
#ifndef _WIN32
    nb_err err;
    nb_err_clear(&err);
    nb_buf *target = nb_buf_new();
    nb_buf *link = nb_buf_new();
    nb_buf *link_copy = nb_buf_new();
    nb_buf *follow_copy = nb_buf_new();
    nb_buf *dir = nb_buf_new();
    nb_buf *dir_link = nb_buf_new();
    nb_buf *dir_copy = nb_buf_new();
    nb_buf *read = nb_buf_new();
    bool ready = target && link && link_copy && follow_copy && dir && dir_link &&
                 dir_copy && read;
    NB_CHECK(ready);
    if (!ready) {
        nb_buf_free(read);
        nb_buf_free(dir_copy);
        nb_buf_free(dir_link);
        nb_buf_free(dir);
        nb_buf_free(follow_copy);
        nb_buf_free(link_copy);
        nb_buf_free(link);
        nb_buf_free(target);
        return;
    }
    NB_CHECK_INT(nb_path_join(target, base, "cs-target"), NB_OK);
    NB_CHECK_INT(nb_path_join(link, base, "cs-link"), NB_OK);
    NB_CHECK_INT(nb_path_join(link_copy, base, "cs-link-copy"), NB_OK);
    NB_CHECK_INT(nb_path_join(follow_copy, base, "cs-follow-copy"), NB_OK);
    NB_CHECK_INT(nb_path_join(dir, base, "cs-dir"), NB_OK);
    NB_CHECK_INT(nb_path_join(dir_link, base, "cs-dir-link"), NB_OK);
    NB_CHECK_INT(nb_path_join(dir_copy, base, "cs-dir-copy"), NB_OK);

    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(target), "payload", 7, &err),
                 NB_OK);
    NB_CHECK_INT(symlink("cs-target", nb_buf_cstr(link)), 0);
    NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(dir), false, &err), NB_OK);
    NB_CHECK_INT(symlink("cs-dir", nb_buf_cstr(dir_link)), 0);

    nb_file_copy_options copy_links = { 0 };
    copy_links.copy_symlinks = true;
    copy_links.overwrite = true;
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(link), nb_buf_cstr(link_copy),
                              &copy_links, &err),
                 NB_OK);
    nb_file_info info;
    memset(&info, 0, sizeof(info));
    NB_CHECK_INT(nb_file_stat(nb_buf_cstr(link_copy), false, &info, &err),
                 NB_OK);
    NB_CHECK(info.is_symlink);
    NB_CHECK_INT(nb_file_readlink(read, nb_buf_cstr(link_copy), &err), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(read), "cs-target");

    nb_file_copy_options follow = { 0 };
    follow.copy_symlinks = false;
    follow.overwrite = true;
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(link), nb_buf_cstr(follow_copy),
                              &follow, &err),
                 NB_OK);
    memset(&info, 0, sizeof(info));
    NB_CHECK_INT(nb_file_stat(nb_buf_cstr(follow_copy), false, &info, &err),
                 NB_OK);
    NB_CHECK(info.is_regular);
    check_contents(nb_buf_cstr(follow_copy), "payload");
    follow.nofollow_source = true;
    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(link), nb_buf_cstr(follow_copy), &follow, &err),
                 NB_ERR_PERMISSION);
    check_contents(nb_buf_cstr(follow_copy), "payload");
    follow.nofollow_source = false;

    NB_CHECK_INT(nb_file_copy(nb_buf_cstr(dir_link), nb_buf_cstr(dir_copy),
                              &follow, &err),
                 NB_ERR_UNSUPPORTED);

    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(link), &err), NB_OK);
    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(link_copy), &err), NB_OK);
    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(follow_copy), &err), NB_OK);
    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(dir_link), &err), NB_OK);
    NB_CHECK_INT(nb_file_rmdir(nb_buf_cstr(dir), &err), NB_OK);
    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(target), &err), NB_OK);
    NB_CHECK_INT(nb_file_remove_all(nb_buf_cstr(dir_copy), 0, &err), NB_OK);
    nb_buf_free(read);
    nb_buf_free(dir_copy);
    nb_buf_free(dir_link);
    nb_buf_free(dir);
    nb_buf_free(follow_copy);
    nb_buf_free(link_copy);
    nb_buf_free(link);
    nb_buf_free(target);
#else
    (void)base;
#endif
}

static void test_remove_all(const char *base)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *tree = nb_buf_new();
    nb_buf *sub = nb_buf_new();
    nb_buf *file = nb_buf_new();
    nb_buf *external = nb_buf_new();
    nb_buf *link = nb_buf_new();
    bool ready = tree && sub && file && external && link;
    NB_CHECK(ready);
    if (!ready) {
        nb_buf_free(link);
        nb_buf_free(external);
        nb_buf_free(file);
        nb_buf_free(sub);
        nb_buf_free(tree);
        return;
    }
    NB_CHECK_INT(nb_path_join(tree, base, "remove-tree"), NB_OK);
    NB_CHECK_INT(nb_path_join(sub, nb_buf_cstr(tree), "sub"), NB_OK);
    NB_CHECK_INT(nb_path_join(file, nb_buf_cstr(sub), "file.txt"), NB_OK);
    NB_CHECK_INT(nb_path_join(external, base, "remove-external"), NB_OK);
    NB_CHECK_INT(nb_path_join(link, nb_buf_cstr(tree), "external-link"),
                 NB_OK);

    NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(tree), false, &err), NB_OK);
    NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(sub), false, &err), NB_OK);
    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(file), "gone", 4, &err),
                 NB_OK);
    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(external), "keep", 4, &err),
                 NB_OK);
#ifndef _WIN32
    NB_CHECK_INT(symlink(nb_buf_cstr(external), nb_buf_cstr(link)), 0);
#endif

    NB_CHECK_INT(nb_file_remove_all(nb_buf_cstr(tree), 0, &err), NB_OK);
    NB_CHECK(!nb_file_exists(nb_buf_cstr(tree)));
    NB_CHECK_INT(nb_file_remove_all(nb_buf_cstr(tree), 0, &err), NB_OK);
    NB_CHECK(nb_file_exists(nb_buf_cstr(external)));

    nb_buf *missing = nb_buf_new();
    NB_CHECK(missing != NULL);
    if (missing) {
        NB_CHECK_INT(nb_path_join(missing, base, "remove-missing"), NB_OK);
        NB_CHECK_INT(nb_file_remove_all(nb_buf_cstr(missing), 0, &err), NB_OK);
        NB_CHECK_INT(nb_file_remove_all("", 0, &err), NB_ERR_INVAL);
        NB_CHECK_INT(nb_file_remove_all(NULL, 0, &err), NB_ERR_INVAL);
        nb_buf_free(missing);
    }

    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(external), &err), NB_OK);
    nb_buf_free(link);
    nb_buf_free(external);
    nb_buf_free(file);
    nb_buf_free(sub);
    nb_buf_free(tree);
}

static void test_executable_and_temp_dir(void)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *path = nb_buf_new();
    NB_CHECK(path != NULL);
    if (!path) {
        return;
    }

    NB_CHECK_INT(nb_path_temp_dir(path, &err), NB_OK);
    NB_CHECK(nb_buf_len(path) > 0);
    NB_CHECK(nb_path_is_absolute(nb_buf_cstr(path)));

    nb_buf_clear(path);
    NB_CHECK_INT(nb_path_executable(path, &err), NB_OK);
    NB_CHECK(nb_buf_len(path) > 0);
    NB_CHECK(nb_path_is_absolute(nb_buf_cstr(path)));
    NB_CHECK(nb_file_exists(nb_buf_cstr(path)));

    NB_CHECK_INT(nb_path_executable(NULL, &err), NB_ERR_INVAL);
    nb_buf_free(path);
}

static void test_native_text_and_scan(const char *base)
{
    nb_buf directory = {0};
    nb_buf file = {0};
    nb_buf relative = {0};
    nb_buf joined = {0};
    nb_dir_listing listing = {0};
    NB_CHECK_INT(nb_path_concat(&directory, base, "scan-contract"), NB_OK);
    NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(&directory), true, NULL), NB_OK);
    NB_CHECK_INT(nb_path_concat(&file, nb_buf_cstr(&directory), "z.txt"), NB_OK);
    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(&file), "z", 1, NULL), NB_OK);
    NB_CHECK_INT(nb_path_concat(&file, nb_buf_cstr(&directory), "a.txt"), NB_OK);
    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(&file), "a", 1, NULL), NB_OK);
    NB_CHECK_INT(nb_path_concat(&file, nb_buf_cstr(&directory), ".hidden"), NB_OK);
    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(&file), "h", 1, NULL), NB_OK);
    NB_CHECK_INT(nb_path_concat(&file, nb_buf_cstr(&directory), "child"), NB_OK);
    NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(&file), false, NULL), NB_OK);

    NB_CHECK_INT(
        nb_dir_scan(nb_buf_cstr(&directory), NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT, &listing, NULL),
        NB_OK);
    NB_CHECK_INT(listing.names.len, 2);
    NB_CHECK_STR(nb_dir_listing_name(&listing, 0), "a.txt");
    NB_CHECK_STR(nb_dir_listing_name(&listing, 1), "z.txt");
    void *storage = listing.names.data;
    NB_CHECK_INT(nb_dir_scan(nb_buf_cstr(&directory), NB_DIR_SCAN_FILES, &listing, NULL),
                 NB_ERR_STATE);
    NB_CHECK(listing.names.data == storage);
    NB_CHECK_INT(nb_file_remove_all(nb_buf_cstr(&directory), 0, NULL), NB_OK);
    NB_CHECK_STR(nb_dir_listing_name(&listing, 0), "a.txt");
    nb_dir_listing_destroy(&listing);
    NB_CHECK_INT(nb_dir_scan(nb_buf_cstr(&directory), NB_DIR_SCAN_FILES, &listing, NULL),
                 NB_ERR_NOT_FOUND);
    NB_CHECK(!listing.initialized);

    NB_CHECK_INT(nb_path_concat(&joined, "root/", "/child"), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(&joined), "root/child");
    NB_CHECK_INT(nb_path_concat(&joined, nb_buf_cstr(&joined), "tail"), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(&joined), "root/child/tail");
    NB_CHECK_INT(nb_path_concat(&joined, base, "missing/leaf"), NB_OK);
    NB_CHECK(nb_path_is_within(nb_buf_cstr(&joined), base, true));
    NB_CHECK(!nb_path_is_within(nb_buf_cstr(&joined), base, false));
    NB_CHECK_INT(nb_path_relative_under(&relative, nb_buf_cstr(&joined), base, true, NULL), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(&relative), "missing/leaf");
    NB_CHECK_INT(nb_path_relative_under(&relative, base, nb_buf_cstr(&joined), true, NULL),
                 NB_ERR_PERMISSION);
    NB_CHECK_STR(nb_buf_cstr(&relative), "missing/leaf");

    char output[4] = "old";
    NB_CHECK_INT(nb_str_copy_to(NB_STR_LIT("long"), output, sizeof(output)), NB_ERR_RANGE);
    NB_CHECK_STR(output, "old");
    NB_CHECK_INT(nb_str_copy_to(nb_str_from_parts(output + 1, 2), output, sizeof(output)), NB_OK);
    NB_CHECK_STR(output, "ld");
    NB_CHECK(nb_str_is_alnum(NB_STR_LIT("Az09_-"), "_-"));
    NB_CHECK(!nb_str_is_alnum(NB_STR_LIT(""), "_-"));
    NB_CHECK(!nb_str_is_alnum(NB_STR_LIT("has space"), "_-"));
    NB_CHECK(nb_ascii_is_alnum_or('_', "_"));
    NB_CHECK(!nb_ascii_is_alnum_or(0, "_"));
    nb_buf_clear(&joined);
    NB_CHECK_INT(nb_random_hex(&joined, 16, NULL), NB_OK);
    NB_CHECK_INT(nb_buf_len(&joined), 32);
    for (size_t i = 0; i < nb_buf_len(&joined); i++) {
        NB_CHECK(strchr("0123456789abcdef", nb_buf_cstr(&joined)[i]) != NULL);
    }

    NB_CHECK_INT(nb_path_concat(&joined, base, "remove-bound/a/b"), NB_OK);
    NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(&joined), true, NULL), NB_OK);
    NB_CHECK_INT(nb_path_concat(&directory, base, "remove-bound"), NB_OK);
    NB_CHECK_INT(nb_file_remove_all(nb_buf_cstr(&directory), 1, NULL), NB_ERR_LOOP);
    NB_CHECK(nb_file_exists(nb_buf_cstr(&joined)));
    NB_CHECK_INT(nb_file_remove_all(nb_buf_cstr(&directory), 0, NULL), NB_OK);
    nb_buf_destroy(&directory);
    nb_buf_destroy(&file);
    nb_buf_destroy(&relative);
    nb_buf_destroy(&joined);
}

void test_fs_extra(void)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *base = nb_buf_new();
    NB_CHECK(base != NULL);
    if (!base) {
        return;
    }
    if (nb_file_temp_dir(base, NULL, NULL, &err) != NB_OK) {
        NB_CHECK(0);
        nb_buf_free(base);
        return;
    }

    test_native_text_and_scan(nb_buf_cstr(base));
    test_stat(nb_buf_cstr(base));
    test_dir_entries(nb_buf_cstr(base));
    test_temp(nb_buf_cstr(base));
    test_sync_dir(nb_buf_cstr(base));
    test_canonical(nb_buf_cstr(base));
#ifndef _WIN32
    test_canonical_long_path(nb_buf_cstr(base));
#endif
    test_locking(nb_buf_cstr(base));
    test_atomic_options(nb_buf_cstr(base));
    test_rename_noreplace(nb_buf_cstr(base));
    test_set_mode(nb_buf_cstr(base));
    test_copy_file(nb_buf_cstr(base));
    test_copy_large_file(nb_buf_cstr(base));
    test_copy_directory(nb_buf_cstr(base));
    test_copy_policy(nb_buf_cstr(base));
    test_symlink_readlink(nb_buf_cstr(base));
    test_copy_symlinks(nb_buf_cstr(base));
    test_remove_all(nb_buf_cstr(base));
    test_executable_and_temp_dir();

    remove_tree(nb_buf_cstr(base));
    NB_CHECK(!nb_file_exists(nb_buf_cstr(base)));
    nb_buf_free(base);
}
