#include "platform/nb_platform.h"

#include "nb/mem.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

bool nb_platform_path_is_sep(char c)
{
    return c == '/';
}

bool nb_platform_path_is_absolute(const char *path)
{
    return path != NULL && path[0] == '/';
}

size_t nb_platform_path_root_len(const char *path)
{
    return path != NULL && path[0] == '/' ? 1u : 0u;
}

size_t nb_platform_path_drive_len(const char *path)
{
    (void)path;
    return 0u;
}

nb_status nb_platform_getcwd(nb_buf *out, nb_err *err)
{
    if (!out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid output buffer");
        return NB_ERR_INVAL;
    }
    nb_buf_clear(out);
    if (!nb_buf_reserve(out, 256u)) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory resolving path");
        return NB_ERR_NOMEM;
    }
    for (;;) {
        errno = 0;
        if (getcwd(out->ptr, out->cap)) {
            out->len = strlen(out->ptr);
            out->off = 0;
            return NB_OK;
        }
        if (errno != ERANGE) {
            int saved = errno;
            nb_err_set_os(err, NB_ERR_IO, saved, "getcwd: %s",
                          strerror(saved));
            return NB_ERR_IO;
        }
        size_t grow = out->cap < 256u ? 256u : out->cap;
        if (!nb_buf_reserve(out, grow)) {
            nb_err_set(err, NB_ERR_NOMEM, "out of memory resolving path");
            return NB_ERR_NOMEM;
        }
    }
}

bool nb_platform_path_prefix_equal(const char *a, const char *b, size_t len)
{
    return a != NULL && b != NULL && memcmp(a, b, len) == 0;
}

nb_status nb_platform_path_canonical(nb_buf *out, const char *path,
                                     nb_err *err)
{
    if (!out || !path || path[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or output buffer");
        return NB_ERR_INVAL;
    }

    nb_buf_clear(out);
    /* realpath() has no destination capacity parameter. Passing NULL makes
     * the library allocate a result with the platform's full path capacity;
     * a fixed buffer can be written past its end for long paths. */
    errno = 0;
    char *result = realpath(path, NULL);
    if (!result) {
        int saved = errno;
        nb_status status = (saved == ENOENT || saved == ENOTDIR) ? NB_ERR_NOT_FOUND : NB_ERR_IO;
        nb_err_set_os(err, status, saved, "realpath '%s': %s", path,
                      strerror(saved));
        return status;
    }

    size_t len = strlen(result);
    bool ok = nb_buf_append(out, result, len);
    free(result);
    if (!ok) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory resolving '%s'", path);
        return NB_ERR_NOMEM;
    }
    return NB_OK;
}

#if defined(__linux__)
static nb_status executable_linux(nb_buf *out, nb_err *err)
{
    size_t cap = 256u;
    for (;;) {
        char *buffer = nb_try_alloc(cap);
        if (!buffer) {
            nb_err_set(err, NB_ERR_NOMEM,
                       "out of memory resolving executable path");
            return NB_ERR_NOMEM;
        }
        ssize_t n = readlink("/proc/self/exe", buffer, cap);
        if (n < 0) {
            int saved = errno;
            nb_free(buffer, cap);
            nb_err_set_os(err, NB_ERR_IO, saved, "readlink /proc/self/exe: %s",
                          strerror(saved));
            return NB_ERR_IO;
        }
        if ((size_t)n < cap) {
            bool ok = nb_buf_append(out, buffer, (size_t)n);
            nb_free(buffer, cap);
            if (!ok) {
                nb_err_set(err, NB_ERR_NOMEM,
                           "out of memory resolving executable path");
                return NB_ERR_NOMEM;
            }
            return NB_OK;
        }
        nb_free(buffer, cap);
        cap *= 2u;
    }
}
#elif defined(__APPLE__)
static nb_status executable_macos(nb_buf *out, nb_err *err)
{
    uint32_t cap = 0;
    (void)_NSGetExecutablePath(NULL, &cap);
    if (cap == 0) {
        nb_err_set(err, NB_ERR_IO, "cannot determine executable path");
        return NB_ERR_IO;
    }
    for (;;) {
        char *buffer = nb_try_alloc(cap);
        if (!buffer) {
            nb_err_set(err, NB_ERR_NOMEM,
                       "out of memory resolving executable path");
            return NB_ERR_NOMEM;
        }
        uint32_t used = cap;
        if (_NSGetExecutablePath(buffer, &used) == 0) {
            nb_status status =
                nb_platform_path_canonical(out, buffer, err);
            nb_free(buffer, cap);
            return status;
        }
        nb_free(buffer, cap);
        if (used <= cap) {
            nb_err_set(err, NB_ERR_IO, "cannot determine executable path");
            return NB_ERR_IO;
        }
        cap = used;
    }
}
#endif

nb_status nb_platform_path_executable(nb_buf *out, nb_err *err)
{
    if (!out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid output buffer");
        return NB_ERR_INVAL;
    }
    nb_buf_clear(out);
#if defined(__linux__)
    return executable_linux(out, err);
#elif defined(__APPLE__)
    return executable_macos(out, err);
#else
    nb_err_set(err, NB_ERR_UNSUPPORTED,
               "executable path lookup is not supported on this platform");
    return NB_ERR_UNSUPPORTED;
#endif
}

nb_status nb_platform_path_temp_dir(nb_buf *out, nb_err *err)
{
    if (!out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid output buffer");
        return NB_ERR_INVAL;
    }
    const char *dir = getenv("TMPDIR");
    if (!dir || dir[0] == '\0') {
        dir = "/tmp";
    }
    return nb_platform_path_canonical(out, dir, err);
}
