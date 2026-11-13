/* macOS exposes flock/renamex_np only with the Darwin namespace enabled. The
 * library build defines _POSIX_C_SOURCE, which otherwise hides them. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "nb/file.h"

#include "nb/mem.h"
#include "nb/str.h"
#include "platform/nb_platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1u
#endif
#endif

static void set_path_err(nb_err *err, nb_status code, const char *what,
                         const char *path)
{
    int saved = errno;
    if (err) {
        nb_err_set_os(err, code, saved, "%s '%s': %s", what, path,
                      strerror(saved));
    }
}

static nb_status fsync_directory(const char *dir, nb_err *err)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        nb_status status = errno == ENOENT ? NB_ERR_NOT_FOUND : NB_ERR_IO;
        set_path_err(err, status, "open directory", dir);
        return status;
    }

    int result;
    do {
        result = fsync(fd);
    } while (result != 0 && errno == EINTR);
    int saved = errno;

    if (close(fd) != 0 && result == 0) {
        saved = errno;
        result = -1;
    }
    if (result == 0) {
        return NB_OK;
    }
    /* Some file systems and macOS versions reject directory fsync. Publishing
     * already happened, so report success where the operation is a no-op. */
    if (saved == EINVAL || saved == ENOTSUP) {
        return NB_OK;
    }
    errno = saved;
    set_path_err(err, NB_ERR_IO, "fsync directory", dir);
    return NB_ERR_IO;
}

/* rename(2) variant that refuses to replace an existing target. */
static int rename_noreplace_raw(const char *from, const char *to)
{
#if defined(__linux__) && defined(SYS_renameat2)
    if (syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to,
                RENAME_NOREPLACE) == 0) {
        return 0;
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return -1;
    }
#elif defined(__APPLE__)
    if (renamex_np(from, to, RENAME_EXCL) == 0) {
        return 0;
    }
    if (errno != ENOTSUP && errno != EINVAL) {
        return -1;
    }
#endif
    /* Hard-link publication is atomic and never replaces to. Directories
     * cannot be linked; the platforms above handle them. */
    if (linkat(AT_FDCWD, from, AT_FDCWD, to, 0) != 0) {
        return -1;
    }
    if (unlink(from) != 0) {
        int saved = errno;
        (void)unlink(to);
        errno = saved;
        return -1;
    }
    return 0;
}

bool nb_file_exists(const char *path)
{
    if (!path) {
        return false;
    }
    struct stat st;
    return stat(path, &st) == 0;
}

bool nb_file_is_dir(const char *path)
{
    if (!path) {
        return false;
    }
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

nb_status nb_file_size(const char *path, uint64_t *size, nb_err *err)
{
    if (!path || !size) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or size output");
        return NB_ERR_INVAL;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        nb_status status = errno == ENOENT ? NB_ERR_NOT_FOUND : NB_ERR_IO;
        set_path_err(err, status, "stat", path);
        return status;
    }
    *size = (uint64_t)st.st_size;
    return NB_OK;
}

nb_status nb_file_stat(const char *path, bool follow_links,
                       nb_file_info *out, nb_err *err)
{
    if (!path || !out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or stat output");
        return NB_ERR_INVAL;
    }
    struct stat st;
    if ((follow_links ? stat(path, &st) : lstat(path, &st)) != 0) {
        nb_status status = errno == ENOENT ? NB_ERR_NOT_FOUND : NB_ERR_IO;
        set_path_err(err, status, "stat", path);
        return status;
    }

    memset(out, 0, sizeof(*out));
    out->size = (uint64_t)st.st_size;
#if defined(__APPLE__)
    int64_t seconds = (int64_t)st.st_mtimespec.tv_sec;
    long nanoseconds = st.st_mtimespec.tv_nsec;
#else
    int64_t seconds = (int64_t)st.st_mtim.tv_sec;
    long nanoseconds = st.st_mtim.tv_nsec;
#endif
    out->mtime_ns = seconds * 1000000000LL + (int64_t)nanoseconds;
    out->mode = (uint32_t)(st.st_mode & 07777);
    out->is_regular = S_ISREG(st.st_mode);
    out->is_dir = S_ISDIR(st.st_mode);
    out->is_symlink = S_ISLNK(st.st_mode);
    out->is_char_device = S_ISCHR(st.st_mode);
    out->is_block_device = S_ISBLK(st.st_mode);
    out->is_fifo = S_ISFIFO(st.st_mode);
    out->is_socket = S_ISSOCK(st.st_mode);
    return NB_OK;
}

nb_status nb_platform_file_read_all(const char *path, nb_buf *out, size_t max_bytes, nb_err *err)
{
    if (!path || !out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or output buffer");
        return NB_ERR_INVAL;
    }
    nb_buf_clear(out);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        nb_status status = errno == ENOENT ? NB_ERR_NOT_FOUND : NB_ERR_IO;
        set_path_err(err, status, "open", path);
        return status;
    }

    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        if (max_bytes && (uint64_t)st.st_size > max_bytes) {
            close(fd);
            nb_err_set(err, NB_ERR_TOO_BIG, "file '%s' exceeds the read limit", path);
            return NB_ERR_TOO_BIG;
        }
        if ((uint64_t)st.st_size > SIZE_MAX) {
            close(fd);
            nb_err_set(err, NB_ERR_RANGE, "file '%s' is too large", path);
            return NB_ERR_RANGE;
        }
        if (!nb_buf_reserve(out, (size_t)st.st_size)) {
            (void)close(fd);
            nb_err_set(err, NB_ERR_NOMEM, "out of memory reading '%s'", path);
            return NB_ERR_NOMEM;
        }
    }

    char chunk[65536];
    for (;;) {
        size_t amount = sizeof(chunk);
        if (max_bytes && max_bytes - out->len < amount) {
            amount = max_bytes - out->len + 1u;
        }
        ssize_t n = read(fd, chunk, amount);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            int saved = errno;
            close(fd);
            errno = saved;
            set_path_err(err, NB_ERR_IO, "read", path);
            return NB_ERR_IO;
        }
        if (n == 0) {
            break;
        }
        if (max_bytes && (size_t)n > max_bytes - out->len) {
            close(fd);
            nb_err_set(err, NB_ERR_TOO_BIG, "file '%s' exceeds the read limit", path);
            return NB_ERR_TOO_BIG;
        }
        if (!nb_buf_append(out, chunk, (size_t)n)) {
            close(fd);
            nb_err_set(err, NB_ERR_NOMEM, "out of memory reading '%s'", path);
            return NB_ERR_NOMEM;
        }
    }

    if (close(fd) != 0) {
        set_path_err(err, NB_ERR_IO, "close", path);
        return NB_ERR_IO;
    }
    return NB_OK;
}

static nb_status write_all_fd(int fd, const void *data, size_t len,
                              const char *path, nb_err *err)
{
    const unsigned char *ptr = (const unsigned char *)data;
    size_t left = len;
    while (left > 0) {
        ssize_t n = write(fd, ptr, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            nb_status status = nb_status_from_errno(errno, NULL);
            set_path_err(err, status, "write", path);
            return status;
        }
        if (n == 0) {
            nb_err_set(err, NB_ERR_IO, "short write to '%s'", path);
            return NB_ERR_IO;
        }
        ptr += (size_t)n;
        left -= (size_t)n;
    }
    return NB_OK;
}

static nb_status write_fd(int fd, const void *data, size_t len,
                          const char *path, nb_err *err)
{
    nb_status status = write_all_fd(fd, data, len, path, err);
    if (status != NB_OK) {
        (void)close(fd);
        return status;
    }
    if (close(fd) != 0) {
        set_path_err(err, NB_ERR_IO, "close", path);
        return NB_ERR_IO;
    }
    return NB_OK;
}

nb_status nb_file_write_all(const char *path, const void *data,
                            size_t len, nb_err *err)
{
    if (!path || (!data && len != 0)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or data");
        return NB_ERR_INVAL;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        set_path_err(err, NB_ERR_IO, "open", path);
        return NB_ERR_IO;
    }
    return write_fd(fd, data, len, path, err);
}

static bool append_temp_template(nb_buf *tmpl, const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        return nb_buf_append_cstr(tmpl, "./.nbase-XXXXXX");
    }
    size_t dir_len = (size_t)(slash - path);
    if (dir_len == 0) {
        return nb_buf_append_cstr(tmpl, "/.nbase-XXXXXX");
    }
    if (!nb_buf_append(tmpl, path, dir_len)) {
        return false;
    }
    if (path[dir_len - 1u] != '/' && !nb_buf_append_char(tmpl, '/')) {
        return false;
    }
    return nb_buf_append_cstr(tmpl, ".nbase-XXXXXX");
}

typedef nb_status (*atomic_writer_fn)(int fd, void *ctx, const char *temp_path, nb_err *err);

static nb_status publish_atomic(const char *path, const nb_file_write_options *options,
                                atomic_writer_fn writer, void *ctx, nb_err *err)
{
    if (options && options->published) {
        *options->published = false;
    }
    /* NULL keeps the documented safe defaults: publish durably and never
     * write through a link or a special file. */
    bool create_only = options != NULL && options->create_only;
    bool sync_dir = options == NULL || options->sync_dir;
    bool follow_symlink = options != NULL && options->follow_symlink;
    uint32_t requested_mode = options ? options->mode & 07777u : 0u;

    struct stat existing;
    bool exists = false;
    int probe = follow_symlink ? stat(path, &existing)
                               : lstat(path, &existing);
    if (probe == 0) {
        exists = true;
    } else if (errno != ENOENT && errno != ENOTDIR) {
        set_path_err(err, NB_ERR_IO, "stat", path);
        return NB_ERR_IO;
    }

    if (create_only && exists) {
        nb_err_set(err, NB_ERR_EXISTS, "path '%s' already exists", path);
        return NB_ERR_EXISTS;
    }
    if (!follow_symlink && exists &&
        (S_ISLNK(existing.st_mode) || !S_ISREG(existing.st_mode))) {
        int os_code = S_ISLNK(existing.st_mode)
                          ? ELOOP
                          : (S_ISDIR(existing.st_mode) ? EISDIR : EINVAL);
        nb_err_set_os(err, NB_ERR_IO, os_code,
                      "refusing to replace non-regular path '%s'", path);
        return NB_ERR_IO;
    }

    nb_buf *tmpl = nb_buf_new();
    if (!tmpl) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory building temporary path");
        return NB_ERR_NOMEM;
    }
    if (!append_temp_template(tmpl, path)) {
        nb_buf_free(tmpl);
        nb_err_set(err, NB_ERR_NOMEM, "out of memory building temporary path");
        return NB_ERR_NOMEM;
    }

    int fd = mkstemp(tmpl->ptr);
    if (fd < 0) {
        set_path_err(err, NB_ERR_IO, "mkstemp", nb_buf_cstr(tmpl));
        nb_buf_free(tmpl);
        return NB_ERR_IO;
    }

    /* mkstemp creates the file as 0600. An explicit mode wins; otherwise an
     * existing target keeps its permission bits and new files get 0644. Do not
     * query umask(): it is process-global and racy. */
    mode_t mode = 0644;
    if (exists) {
        mode = existing.st_mode & 07777;
    } else if (requested_mode == 0 && options && options->new_file_mode != 0) {
        requested_mode = options->new_file_mode & 07777u;
    }
    if (requested_mode != 0) {
        mode = (mode_t)requested_mode;
    }
    if (fchmod(fd, mode) != 0) {
        int saved = errno;
        (void)close(fd);
        (void)unlink(tmpl->ptr);
        errno = saved;
        set_path_err(err, NB_ERR_IO, "fchmod", nb_buf_cstr(tmpl));
        nb_buf_free(tmpl);
        return NB_ERR_IO;
    }

    nb_status status = writer(fd, ctx, nb_buf_cstr(tmpl), err);
    if (status != NB_OK) {
        (void)close(fd);
        (void)unlink(tmpl->ptr);
        nb_buf_free(tmpl);
        return status;
    }

    while (fsync(fd) != 0) {
        if (errno == EINTR) {
            continue;
        }
        int saved = errno;
        (void)close(fd);
        (void)unlink(tmpl->ptr);
        errno = saved;
        set_path_err(err, NB_ERR_IO, "fsync", nb_buf_cstr(tmpl));
        nb_buf_free(tmpl);
        return NB_ERR_IO;
    }

    if (close(fd) != 0) {
        int saved = errno;
        (void)unlink(tmpl->ptr);
        errno = saved;
        set_path_err(err, NB_ERR_IO, "close", nb_buf_cstr(tmpl));
        nb_buf_free(tmpl);
        return NB_ERR_IO;
    }

    int moved = create_only ? rename_noreplace_raw(tmpl->ptr, path)
                            : rename(tmpl->ptr, path);
    if (moved != 0) {
        int saved = errno;
        (void)unlink(tmpl->ptr);
        errno = saved;
        nb_status publish_status =
            create_only && (saved == EEXIST || saved == ENOTEMPTY)
                ? NB_ERR_EXISTS
                : NB_ERR_IO;
        if (err) {
            nb_err_set_os(err, publish_status, saved,
                          "rename '%s' to '%s': %s", nb_buf_cstr(tmpl), path,
                          strerror(saved));
        }
        nb_buf_free(tmpl);
        return publish_status;
    }
    nb_buf_free(tmpl);
    if (options && options->published) {
        *options->published = true;
    }

    if (sync_dir) {
        nb_buf parent;
        nb_buf_init(&parent);
        status = nb_path_dirname(&parent, path);
        if (status == NB_OK) {
            status = fsync_directory(nb_buf_cstr(&parent), err);
        } else {
            nb_err_set(err, NB_ERR_NOMEM,
                       "out of memory resolving parent of '%s'", path);
        }
        nb_buf_destroy(&parent);
        if (status != NB_OK) {
            return status;
        }
    }
    return NB_OK;
}

typedef struct atomic_buffer {
    const void *data;
    size_t len;
} atomic_buffer;

static nb_status atomic_write_buffer(int fd, void *ctx, const char *temp_path, nb_err *err)
{
    const atomic_buffer *buffer = ctx;
    return write_all_fd(fd, buffer->data, buffer->len, temp_path, err);
}

nb_status nb_file_write_all_atomic(const char *path, const void *data, size_t len,
                                   const nb_file_write_options *options, nb_err *err)
{
    if (!path || path[0] == '\0' || (!data && len != 0)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or data");
        return NB_ERR_INVAL;
    }
    atomic_buffer buffer = {.data = data, .len = len};
    return publish_atomic(path, options, atomic_write_buffer, &buffer, err);
}

nb_status nb_file_append(const char *path, const void *data,
                         size_t len, nb_err *err)
{
    if (!path || (!data && len != 0)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or data");
        return NB_ERR_INVAL;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        set_path_err(err, NB_ERR_IO, "open", path);
        return NB_ERR_IO;
    }
    return write_fd(fd, data, len, path, err);
}

nb_status nb_file_remove(const char *path, nb_err *err)
{
    if (!path) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NB_ERR_INVAL;
    }
    if (unlink(path) != 0) {
        nb_status status = errno == ENOENT ? NB_ERR_NOT_FOUND : NB_ERR_IO;
        set_path_err(err, status, "remove", path);
        return status;
    }
    return NB_OK;
}

nb_status nb_file_rename(const char *from, const char *to, nb_err *err)
{
    if (!from || !to) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NB_ERR_INVAL;
    }
    if (rename(from, to) != 0) {
        set_path_err(err, NB_ERR_IO, "rename", from);
        return NB_ERR_IO;
    }
    return NB_OK;
}

nb_status nb_file_rename_noreplace(const char *from, const char *to,
                                   nb_err *err)
{
    if (!from || !to) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NB_ERR_INVAL;
    }
    if (rename_noreplace_raw(from, to) != 0) {
        int saved = errno;
        nb_status status = (saved == EEXIST || saved == ENOTEMPTY)
                               ? NB_ERR_EXISTS
                               : NB_ERR_IO;
        errno = saved;
        set_path_err(err, status, "rename", from);
        return status;
    }
    return NB_OK;
}

nb_status nb_file_mkdir(const char *path, bool recursive, nb_err *err)
{
    if (!path || path[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NB_ERR_INVAL;
    }
    if (!recursive) {
        if (mkdir(path, 0755) != 0) {
            if (errno == EEXIST && nb_file_is_dir(path)) {
                return NB_OK;
            }
            nb_status status = errno == EEXIST ? NB_ERR_EXISTS : NB_ERR_IO;
            set_path_err(err, status, "mkdir", path);
            return status;
        }
        return NB_OK;
    }

    size_t len = strlen(path);
    char *copy = nb_try_alloc(len + 1u);
    if (!copy) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory creating directory");
        return NB_ERR_NOMEM;
    }
    memcpy(copy, path, len + 1u);
    while (len > 1u && copy[len - 1u] == '/') {
        copy[--len] = '\0';
    }

    for (char *p = copy + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
            int saved = errno;
            nb_free(copy, strlen(path) + 1u);
            errno = saved;
            set_path_err(err, NB_ERR_IO, "mkdir", path);
            return NB_ERR_IO;
        }
        *p = '/';
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
        int saved = errno;
        nb_free(copy, strlen(path) + 1u);
        errno = saved;
        set_path_err(err, NB_ERR_IO, "mkdir", path);
        return NB_ERR_IO;
    }
    bool is_dir = nb_file_is_dir(copy);
    nb_free(copy, strlen(path) + 1u);
    if (!is_dir) {
        nb_err_set(err, NB_ERR_IO, "path '%s' is not a directory", path);
        return NB_ERR_IO;
    }
    return NB_OK;
}

nb_status nb_file_rmdir(const char *path, nb_err *err)
{
    if (!path) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NB_ERR_INVAL;
    }
    if (rmdir(path) != 0) {
        nb_status status = errno == ENOENT ? NB_ERR_NOT_FOUND : NB_ERR_IO;
        set_path_err(err, status, "rmdir", path);
        return status;
    }
    return NB_OK;
}

nb_status nb_file_temp_in(nb_buf *path_out, const char *dir, int *fd_out, nb_err *err)
{
    if (!path_out || !dir || dir[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid output buffer or directory");
        return NB_ERR_INVAL;
    }
    if (fd_out && *fd_out != -1) {
        nb_err_set(err, NB_ERR_STATE, "temporary descriptor output is occupied");
        return NB_ERR_STATE;
    }
    nb_buf *tmpl = nb_buf_new();
    if (!tmpl) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory creating temporary path");
        return NB_ERR_NOMEM;
    }
    size_t dir_len = strlen(dir);
    bool needs_slash = dir[dir_len - 1u] != '/';
    if (!nb_buf_printf(tmpl, "%s%snbase-XXXXXX", dir,
                       needs_slash ? "/" : "")) {
        nb_buf_free(tmpl);
        nb_err_set(err, NB_ERR_NOMEM, "out of memory creating temporary path");
        return NB_ERR_NOMEM;
    }
    int fd = mkstemp(tmpl->ptr);
    if (fd < 0) {
        int captured = errno;
        nb_status status = nb_status_from_errno(captured, NULL);
        nb_err_set_os(err, status, captured, "create temporary file '%s'", tmpl->ptr);
        nb_buf_free(tmpl);
        return status;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        nb_status status = nb_status_from_errno(errno, err);
        (void)close(fd);
        (void)unlink(tmpl->ptr);
        nb_buf_free(tmpl);
        return status;
    }
    if (!fd_out && close(fd) != 0) {
        int captured = errno;
        nb_status status = nb_status_from_errno(captured, NULL);
        nb_err_set_os(err, status, captured, "close temporary file '%s'", tmpl->ptr);
        (void)unlink(tmpl->ptr);
        nb_buf_free(tmpl);
        return status;
    }
    if (fd_out)
        *fd_out = fd;
    nb_buf_move(path_out, tmpl);
    nb_buf_free(tmpl);
    return NB_OK;
}

nb_status nb_file_temp(nb_buf *path_out, nb_err *err)
{
    if (!path_out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid output buffer");
        return NB_ERR_INVAL;
    }
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || tmpdir[0] == '\0') {
        tmpdir = "/tmp";
    }
    return nb_file_temp_in(path_out, tmpdir, NULL, err);
}

nb_status nb_file_temp_dir(nb_buf *path_out, const char *dir, const char *prefix, nb_err *err)
{
    if (!path_out || (dir && !*dir) || (prefix && (strchr(prefix, '/') || strchr(prefix, '\\')))) {
        nb_err_set(err, NB_ERR_INVAL, "invalid output buffer");
        return NB_ERR_INVAL;
    }
    const char *tmpdir = dir ? dir : getenv("TMPDIR");
    if (!tmpdir || tmpdir[0] == '\0') {
        tmpdir = "/tmp";
    }

    nb_buf *candidate = nb_buf_new();
    if (!candidate) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory creating temporary path");
        return NB_ERR_NOMEM;
    }
    size_t dir_len = strlen(tmpdir);
    bool needs_slash = tmpdir[dir_len - 1u] != '/';
    for (unsigned attempt = 0; attempt < 64u; attempt++) {
        unsigned char random[8];
        nb_status status =
            nb_platform_random_bytes(random, sizeof(random), err);
        if (status != NB_OK) {
            nb_buf_free(candidate);
            return status;
        }
        nb_buf_clear(candidate);
        if (!nb_buf_printf(candidate, "%s%s%s", tmpdir, needs_slash ? "/" : "",
                           prefix ? prefix : "nbase-")) {
            nb_buf_free(candidate);
            nb_err_set(err, NB_ERR_NOMEM,
                       "out of memory creating temporary path");
            return NB_ERR_NOMEM;
        }
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < sizeof(random); i++) {
            char pair[2] = { hex[random[i] >> 4], hex[random[i] & 0x0fu] };
            if (!nb_buf_append(candidate, pair, sizeof(pair))) {
                nb_buf_free(candidate);
                nb_err_set(err, NB_ERR_NOMEM,
                           "out of memory creating temporary path");
                return NB_ERR_NOMEM;
            }
        }
        if (mkdir(nb_buf_cstr(candidate), 0700) == 0) {
            nb_buf_move(path_out, candidate);
            nb_buf_free(candidate);
            return NB_OK;
        }
        if (errno != EEXIST) {
            int captured = errno;
            status = nb_status_from_errno(captured, NULL);
            nb_err_set_os(err, status, captured, "create temporary directory '%s'",
                          nb_buf_cstr(candidate));
            nb_buf_free(candidate);
            return status;
        }
    }
    nb_buf_free(candidate);
    nb_err_set(err, NB_ERR_EXISTS,
               "could not create a temporary directory in '%s'", tmpdir);
    return NB_ERR_EXISTS;
}

nb_status nb_file_sync_dir(const char *dir, nb_err *err)
{
    if (!dir || dir[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid directory");
        return NB_ERR_INVAL;
    }
    return fsync_directory(dir, err);
}

struct nb_file_lock {
    int fd;
};

nb_status nb_file_lock_acquire(const char *path, bool exclusive,
                               bool blocking, nb_file_lock **out,
                               nb_err *err)
{
    if (!path || path[0] == '\0' || !out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid lock path or output");
        return NB_ERR_INVAL;
    }
    *out = NULL;

    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        set_path_err(err, NB_ERR_IO, "open lock", path);
        return NB_ERR_IO;
    }

    int operation = (exclusive ? LOCK_EX : LOCK_SH) | (blocking ? 0 : LOCK_NB);
    int result;
    do {
        result = flock(fd, operation);
    } while (result != 0 && errno == EINTR && blocking);
    if (result != 0) {
        int saved = errno;
        (void)close(fd);
        if (saved == EAGAIN || saved == EWOULDBLOCK) {
            nb_err_set(err, NB_ERR_AGAIN, "lock '%s' is held by another owner",
                       path);
            return NB_ERR_AGAIN;
        }
        errno = saved;
        set_path_err(err, NB_ERR_IO, "lock", path);
        return NB_ERR_IO;
    }

    nb_file_lock *lock = nb_try_alloc(sizeof(*lock));
    if (!lock) {
        (void)flock(fd, LOCK_UN);
        (void)close(fd);
        nb_err_set(err, NB_ERR_NOMEM, "out of memory acquiring lock");
        return NB_ERR_NOMEM;
    }
    lock->fd = fd;
    *out = lock;
    return NB_OK;
}

void nb_file_lock_release(nb_file_lock *lock)
{
    if (!lock) {
        return;
    }
    (void)flock(lock->fd, LOCK_UN);
    (void)close(lock->fd);
    nb_free(lock, sizeof(*lock));
}

struct nb_dir {
    DIR *dir;
    nb_status status;
};

nb_dir *nb_dir_open(const char *path, nb_err *err)
{
    if (!path) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NULL;
    }
    DIR *handle = opendir(path);
    if (!handle) {
        int captured = errno;
        nb_err_set_os(err, nb_status_from_errno(captured, NULL), captured,
                      "opendir failed for '%s'", path);
        return NULL;
    }
    nb_dir *dir = nb_try_alloc(sizeof(*dir));
    if (!dir) {
        (void)closedir(handle);
        nb_err_set(err, NB_ERR_NOMEM, "out of memory opening directory");
        return NULL;
    }
    dir->dir = handle;
    dir->status = NB_OK;
    return dir;
}

static nb_dir_entry_type dir_type_from_mode(mode_t mode)
{
    if (S_ISREG(mode)) {
        return NB_DIR_ENTRY_FILE;
    }
    if (S_ISDIR(mode)) {
        return NB_DIR_ENTRY_DIR;
    }
    if (S_ISLNK(mode)) {
        return NB_DIR_ENTRY_SYMLINK;
    }
    if (S_ISCHR(mode)) {
        return NB_DIR_ENTRY_CHAR;
    }
    if (S_ISBLK(mode)) {
        return NB_DIR_ENTRY_BLOCK;
    }
    if (S_ISFIFO(mode)) {
        return NB_DIR_ENTRY_FIFO;
    }
    if (S_ISSOCK(mode)) {
        return NB_DIR_ENTRY_SOCKET;
    }
    return NB_DIR_ENTRY_UNKNOWN;
}

static nb_dir_entry_type dir_type_from_dirent(unsigned char type)
{
#ifdef DT_REG
    switch (type) {
    case DT_REG:
        return NB_DIR_ENTRY_FILE;
    case DT_DIR:
        return NB_DIR_ENTRY_DIR;
    case DT_LNK:
        return NB_DIR_ENTRY_SYMLINK;
    case DT_CHR:
        return NB_DIR_ENTRY_CHAR;
    case DT_BLK:
        return NB_DIR_ENTRY_BLOCK;
    case DT_FIFO:
        return NB_DIR_ENTRY_FIFO;
    case DT_SOCK:
        return NB_DIR_ENTRY_SOCKET;
    default:
        break;
    }
#else
    (void)type;
#endif
    return NB_DIR_ENTRY_UNKNOWN;
}

const char *nb_dir_next(nb_dir *dir, nb_dir_entry *entry_out)
{
    if (!dir || !dir->dir) {
        return NULL;
    }
    errno = 0;
    for (;;) {
        struct dirent *item = readdir(dir->dir);
        if (!item) {
            if (errno != 0) {
                dir->status = NB_ERR_IO;
            }
            return NULL;
        }
        if (strcmp(item->d_name, ".") == 0 ||
            strcmp(item->d_name, "..") == 0) {
            continue;
        }
        if (entry_out) {
            entry_out->name = item->d_name;
            entry_out->type = dir_type_from_dirent(item->d_type);
            if (entry_out->type == NB_DIR_ENTRY_UNKNOWN) {
                struct stat info;
                if (fstatat(dirfd(dir->dir), item->d_name, &info,
                            AT_SYMLINK_NOFOLLOW) != 0) {
                    dir->status = NB_ERR_IO;
                    return NULL;
                }
                entry_out->type = dir_type_from_mode(info.st_mode);
            }
        }
        return item->d_name;
    }
}

nb_status nb_dir_status(const nb_dir *dir)
{
    return dir ? dir->status : NB_ERR_INVAL;
}

void nb_dir_close(nb_dir *dir)
{
    if (!dir) {
        return;
    }
    if (dir->dir) {
        closedir(dir->dir);
    }
    nb_free(dir, sizeof(*dir));
}

nb_status nb_file_set_mode(const char *path, uint32_t mode, nb_err *err)
{
    if (!path || path[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NB_ERR_INVAL;
    }
    if (chmod(path, (mode_t)(mode & 07777u)) != 0) {
        nb_status status = errno == ENOENT ? NB_ERR_NOT_FOUND : NB_ERR_IO;
        set_path_err(err, status, "chmod", path);
        return status;
    }
    return NB_OK;
}

nb_status nb_file_symlink(const char *target, const char *link_path,
                          nb_err *err)
{
    if (!target || !link_path || link_path[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid symbolic link path");
        return NB_ERR_INVAL;
    }
    if (symlink(target, link_path) != 0) {
        nb_status status = NB_ERR_IO;
        if (errno == EEXIST) {
            status = NB_ERR_EXISTS;
        } else if (errno == ENOENT || errno == ENOTDIR) {
            status = NB_ERR_NOT_FOUND;
        }
        set_path_err(err, status, "symlink", link_path);
        return status;
    }
    return NB_OK;
}

nb_status nb_file_readlink(nb_buf *out, const char *path, nb_err *err)
{
    if (!out || !path || path[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or output buffer");
        return NB_ERR_INVAL;
    }

    nb_buf temporary;
    nb_buf_init(&temporary);
    size_t cap = 256u;
    for (;;) {
        char *buffer = nb_try_alloc(cap);
        if (!buffer) {
            nb_buf_destroy(&temporary);
            nb_err_set(err, NB_ERR_NOMEM,
                       "out of memory reading symbolic link '%s'", path);
            return NB_ERR_NOMEM;
        }
        ssize_t length = readlink(path, buffer, cap);
        if (length < 0) {
            int saved = errno;
            nb_free(buffer, cap);
            nb_buf_destroy(&temporary);
            nb_status status = (saved == ENOENT || saved == ENOTDIR)
                                   ? NB_ERR_NOT_FOUND
                                   : (saved == EINVAL ? NB_ERR_INVAL
                                                      : NB_ERR_IO);
            errno = saved;
            set_path_err(err, status, "readlink", path);
            return status;
        }
        if ((size_t)length < cap) {
            bool ok = nb_buf_append(&temporary, buffer, (size_t)length);
            nb_free(buffer, cap);
            if (!ok) {
                nb_buf_destroy(&temporary);
                nb_err_set(err, NB_ERR_NOMEM,
                           "out of memory reading symbolic link '%s'", path);
                return NB_ERR_NOMEM;
            }
            nb_buf_move(out, &temporary);
            nb_buf_destroy(&temporary);
            return NB_OK;
        }
        nb_free(buffer, cap);
        cap *= 2u;
    }
}

typedef struct atomic_copy {
    int source_fd;
    const char *source_path;
} atomic_copy;

static nb_status atomic_copy_file(int fd, void *ctx, const char *temp_path, nb_err *err)
{
    const atomic_copy *copy = ctx;
    unsigned char buffer[64u * 1024u];

    for (;;) {
        ssize_t got;
        do {
            got = read(copy->source_fd, buffer, sizeof(buffer));
        } while (got < 0 && errno == EINTR);
        if (got < 0) {
            set_path_err(err, NB_ERR_IO, "read", copy->source_path);
            return NB_ERR_IO;
        }
        if (got == 0) {
            return NB_OK;
        }
        nb_status status = write_all_fd(fd, buffer, (size_t)got, temp_path, err);
        if (status != NB_OK) {
            return status;
        }
    }
}

static nb_status copy_regular_file(const char *src, const char *dst,
                                   const nb_file_copy_options *options,
                                   uint32_t source_mode, nb_err *err)
{
    struct stat dst_info;
    if (lstat(dst, &dst_info) == 0) {
        if (S_ISDIR(dst_info.st_mode)) {
            nb_err_set(err, NB_ERR_EXISTS, "destination '%s' is a directory", dst);
            return NB_ERR_EXISTS;
        }
        if (!options->overwrite) {
            nb_err_set(err, NB_ERR_EXISTS, "path '%s' already exists", dst);
            return NB_ERR_EXISTS;
        }
    }

    int source_fd = open(src, O_RDONLY | O_CLOEXEC | (options->nofollow_source ? O_NOFOLLOW : 0));
    if (source_fd < 0) {
        set_path_err(err, NB_ERR_IO, "open", src);
        return NB_ERR_IO;
    }

    struct stat opened_source;
    if (fstat(source_fd, &opened_source) != 0 || !S_ISREG(opened_source.st_mode)) {
        int saved_errno = errno;
        close(source_fd);
        nb_err_set_os(err, NB_ERR_IO, saved_errno, "source is not a readable regular file");
        return NB_ERR_IO;
    }

    nb_file_write_options write_options = { 0 };
    write_options.mode =
        options->preserve_mode ? (source_mode & 07777u) : 0u;
    write_options.new_file_mode = options->new_file_mode;
    write_options.create_only = !options->overwrite;
    /* Publishing is the durability boundary for a copied file. */
    write_options.sync_dir = true;
    /* A rename replaces a destination link itself, not its referent. */
    write_options.follow_symlink = true;
    atomic_copy copy = {.source_fd = source_fd, .source_path = src};
    nb_status status = publish_atomic(dst, &write_options, atomic_copy_file, &copy, err);
    (void)close(source_fd);
    return status;
}

static nb_status copy_entry(const char *src, const char *dst, const nb_file_copy_options *options,
                            size_t depth, nb_err *err);

static nb_status copy_directory(const char *src, const char *dst,
                                const nb_file_copy_options *options, uint32_t source_mode,
                                size_t depth, nb_err *err)
{
    if (!options->recursive) {
        nb_err_set(err, NB_ERR_UNSUPPORTED,
                   "copying directory '%s' requires recursive mode", src);
        return NB_ERR_UNSUPPORTED;
    }

    struct stat dst_info;
    bool dst_exists = lstat(dst, &dst_info) == 0;
    if (!dst_exists && errno != ENOENT) {
        set_path_err(err, NB_ERR_IO, "stat", dst);
        return NB_ERR_IO;
    }

    bool created = false;
    if (dst_exists) {
        if (!options->overwrite) {
            nb_err_set(err, NB_ERR_EXISTS, "path '%s' already exists", dst);
            return NB_ERR_EXISTS;
        }
        if (!S_ISDIR(dst_info.st_mode)) {
            if (!options->overwrite) {
                nb_err_set(err, NB_ERR_EXISTS, "path '%s' already exists",
                           dst);
                return NB_ERR_EXISTS;
            }
            nb_status removed = nb_file_remove(dst, err);
            if (removed != NB_OK) {
                return removed;
            }
            dst_exists = false;
        }
    }
    if (!dst_exists) {
        if (mkdir(dst, options->directory_mode ? (mode_t)(options->directory_mode & 07777u)
                                               : 0755) != 0) {
            nb_status status = errno == EEXIST ? NB_ERR_EXISTS : NB_ERR_IO;
            set_path_err(err, status, "mkdir", dst);
            return status;
        }
        created = true;
    }

    nb_dir *dir = nb_dir_open(src, err);
    if (!dir) {
        nb_status status = err ? (nb_status)err->code : NB_ERR_IO;
        if (created) {
            (void)nb_file_remove_all(dst, 0, NULL);
        }
        return status;
    }

    nb_buf *src_child = nb_buf_new();
    nb_buf *dst_child = nb_buf_new();
    nb_status status = NB_OK;
    if (!src_child || !dst_child) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory copying directory");
        status = NB_ERR_NOMEM;
    }
    while (status == NB_OK) {
        nb_dir_entry entry;
        const char *name = nb_dir_next(dir, &entry);
        if (!name) {
            break;
        }
        if (nb_path_join(src_child, src, name) != NB_OK ||
            nb_path_join(dst_child, dst, name) != NB_OK) {
            nb_err_set(err, NB_ERR_NOMEM,
                       "out of memory copying directory '%s'", src);
            status = NB_ERR_NOMEM;
            break;
        }
        status =
            copy_entry(nb_buf_cstr(src_child), nb_buf_cstr(dst_child), options, depth + 1u, err);
    }
    if (status == NB_OK) {
        status = nb_dir_status(dir);
        if (status != NB_OK) {
            nb_err_set(err, status, "failed to read directory '%s'", src);
        }
    }
    nb_dir_close(dir);
    nb_buf_free(dst_child);
    nb_buf_free(src_child);

    if (status == NB_OK && (options->preserve_mode || options->directory_mode)) {
        uint32_t mode = options->directory_mode ? options->directory_mode : source_mode;
        if (chmod(dst, (mode_t)(mode & 07777u)) != 0) {
            set_path_err(err, NB_ERR_IO, "chmod", dst);
            status = NB_ERR_IO;
        }
    }
    if (status != NB_OK && created) {
        (void)nb_file_remove_all(dst, 0, NULL);
    }
    return status;
}

static nb_status copy_entry_run(const char *src, const char *dst,
                                const nb_file_copy_options *options, size_t depth, nb_buf *target,
                                nb_err *err)
{
    nb_file_copy_entry entry = {.depth = depth};
    nb_status status = nb_file_stat(src, false, &entry.info, err);
    if (status != NB_OK)
        return status;
    if (entry.info.is_symlink && options->copy_symlinks) {
        status = nb_file_readlink(target, src, err);
        if (status != NB_OK)
            return status;
        entry.symlink_target = nb_buf_cstr(target);
    }
    bool include = true;
    if (options->filter) {
        status = options->filter(src, dst, &entry, &include, options->filter_context, err);
        if (status != NB_OK || !include)
            return status;
    }
    if (options->max_depth && depth > options->max_depth) {
        nb_err_set(err, NB_ERR_LOOP, "recursive copy depth exceeded");
        return NB_ERR_LOOP;
    }
    if (entry.info.is_symlink) {
        if (options->nofollow_source) {
            nb_err_set(err, NB_ERR_PERMISSION, "source symlink is forbidden");
            return NB_ERR_PERMISSION;
        }
        if (options->copy_symlinks) {
            struct stat dst_info;
            if (lstat(dst, &dst_info) == 0) {
                if (!options->overwrite || S_ISDIR(dst_info.st_mode)) {
                    nb_err_set(err, NB_ERR_EXISTS, "path '%s' already exists", dst);
                    return NB_ERR_EXISTS;
                }
                status = nb_file_remove(dst, err);
                if (status != NB_OK)
                    return status;
            } else if (errno != ENOENT) {
                set_path_err(err, NB_ERR_IO, "stat", dst);
                return NB_ERR_IO;
            }
            return nb_file_symlink(entry.symlink_target, dst, err);
        }
        nb_file_info referent;
        status = nb_file_stat(src, true, &referent, err);
        if (status != NB_OK)
            return status;
        if (!referent.is_regular) {
            nb_err_set(err, NB_ERR_UNSUPPORTED, "refusing to follow non-file link '%s'", src);
            return NB_ERR_UNSUPPORTED;
        }
        return copy_regular_file(src, dst, options, referent.mode, err);
    }
    if (entry.info.is_regular) {
        nb_file_copy_options regular = *options;
        regular.nofollow_source = true;
        return copy_regular_file(src, dst, &regular, entry.info.mode, err);
    }
    if (entry.info.is_dir)
        return copy_directory(src, dst, options, entry.info.mode, depth, err);
    nb_err_set(err, NB_ERR_UNSUPPORTED, "cannot copy special file '%s'", src);
    return NB_ERR_UNSUPPORTED;
}

static nb_status copy_entry(const char *src, const char *dst, const nb_file_copy_options *options,
                            size_t depth, nb_err *err)
{
    nb_buf target;
    nb_buf_init(&target);
    nb_status status = copy_entry_run(src, dst, options, depth, &target, err);
    nb_buf_destroy(&target);
    return status;
}

nb_status nb_file_copy(const char *src, const char *dst,
                       const nb_file_copy_options *options, nb_err *err)
{
    if (!src || !dst || src[0] == '\0' || dst[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid copy path");
        return NB_ERR_INVAL;
    }
    nb_file_copy_options effective = {
        .recursive = false,
        .preserve_mode = true,
        .copy_symlinks = true,
        .overwrite = false,
    };
    if (options) {
        effective = *options;
    }
    return copy_entry(src, dst, &effective, 0, err);
}

static nb_status remove_all_entry(const char *path, size_t remaining, nb_err *err)
{
    if (remaining == 0) {
        nb_err_set(err, NB_ERR_LOOP, "recursive removal depth exceeded");
        return NB_ERR_LOOP;
    }
    struct stat info;
    if (lstat(path, &info) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return NB_OK;
        }
        set_path_err(err, NB_ERR_IO, "stat", path);
        return NB_ERR_IO;
    }

    if (S_ISDIR(info.st_mode)) {
        nb_err open_error;
        nb_err_clear(&open_error);
        nb_dir *dir = nb_dir_open(path, &open_error);
        if (!dir) {
            if (open_error.code == NB_ERR_NOT_FOUND) {
                return NB_OK;
            }
            if (err) {
                *err = open_error;
            }
            return (nb_status)open_error.code;
        }
        nb_buf *child = nb_buf_new();
        nb_status status = NB_OK;
        if (!child) {
            nb_err_set(err, NB_ERR_NOMEM,
                       "out of memory removing directory '%s'", path);
            status = NB_ERR_NOMEM;
        }
        while (status == NB_OK) {
            const char *name = nb_dir_next(dir, NULL);
            if (!name) {
                break;
            }
            if (nb_path_join(child, path, name) != NB_OK) {
                nb_err_set(err, NB_ERR_NOMEM,
                           "out of memory removing directory '%s'", path);
                status = NB_ERR_NOMEM;
                break;
            }
            status = remove_all_entry(nb_buf_cstr(child), remaining - 1, err);
        }
        if (status == NB_OK) {
            status = nb_dir_status(dir);
            if (status != NB_OK) {
                nb_err_set(err, status, "failed to read directory '%s'",
                           path);
            }
        }
        nb_dir_close(dir);
        nb_buf_free(child);
        if (status != NB_OK) {
            return status;
        }
        if (rmdir(path) != 0) {
            if (errno == ENOENT) {
                return NB_OK;
            }
            set_path_err(err, NB_ERR_IO, "rmdir", path);
            return NB_ERR_IO;
        }
        return NB_OK;
    }

    if (unlink(path) != 0) {
        if (errno == ENOENT) {
            return NB_OK;
        }
        set_path_err(err, NB_ERR_IO, "remove", path);
        return NB_ERR_IO;
    }
    return NB_OK;
}

nb_status nb_file_remove_all(const char *path, size_t max_depth, nb_err *err)
{
    size_t remaining = max_depth && max_depth < SIZE_MAX ? max_depth + 1 : SIZE_MAX;
    if (!path || path[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NB_ERR_INVAL;
    }

    size_t length = strlen(path);
    while (length > 1u && path[length - 1u] == '/') {
        length--;
    }
    if (length == strlen(path)) {
        return remove_all_entry(path, remaining, err);
    }
    char *trimmed = nb_try_alloc(length + 1u);
    if (!trimmed) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory removing '%s'", path);
        return NB_ERR_NOMEM;
    }
    memcpy(trimmed, path, length);
    trimmed[length] = '\0';
    nb_status status = remove_all_entry(trimmed, remaining, err);
    nb_free(trimmed, length + 1u);
    return status;
}
