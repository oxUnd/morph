#include "nb/file.h"

#include "nb/mem.h"
#include "nb/cleanup.h"
#include "platform/nb_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <wchar.h>

#define NB_IO_CHUNK 65536u
#define NB_TEMP_ATTEMPTS 64u

static bool win_is_sep(wchar_t c)
{
    return c == L'/' || c == L'\\';
}

static bool win_is_alpha(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

static wchar_t win_upper(wchar_t c)
{
    return (c >= L'a' && c <= L'z') ? (wchar_t)(c - L'a' + L'A') : c;
}

static nb_status win_error_status(DWORD code)
{
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        return NB_ERR_NOT_FOUND;
    }
    if (code == ERROR_DIRECTORY) {
        return NB_ERR_NOT_DIR;
    }
    if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
        return NB_ERR_EXISTS;
    }
    return NB_ERR_IO;
}

static void report_win_error(nb_err *err, DWORD code, const char *what,
                             const char *path)
{
    if (!err) {
        return;
    }
    nb_err_set_os(err, win_error_status(code), (int)code,
                  "%s '%s' failed (win32 error %lu)", what,
                  path ? path : "", (unsigned long)code);
}

/* Write-side failures use NB_ERR_IO regardless of whether the missing object
 * is the target, its parent directory, or a temporary file. This matches the
 * POSIX backend's open/mkstemp/rename error contract. */
static void report_win_io_error(nb_err *err, DWORD code, const char *what,
                                const char *path)
{
    if (!err) {
        return;
    }
    nb_err_set_os(err, NB_ERR_IO, (int)code,
                  "%s '%s' failed (win32 error %lu)", what,
                  path ? path : "", (unsigned long)code);
}

static void wide_free(wchar_t *wide)
{
    if (wide) {
        nb_free(wide, (wcslen(wide) + 1u) * sizeof(wchar_t));
    }
}

static nb_status utf8_to_wide(const char *text, wchar_t **out, nb_err *err)
{
    *out = NULL;
    if (!text) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NB_ERR_INVAL;
    }

    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                                     NULL, 0);
    if (needed <= 0) {
        DWORD code = GetLastError();
        if (code == ERROR_NO_UNICODE_TRANSLATION) {
            nb_err_set_os(err, NB_ERR_INVAL, (int)code,
                          "path is not valid UTF-8");
            return NB_ERR_INVAL;
        }
        report_win_error(err, code, "MultiByteToWideChar", text);
        return win_error_status(code);
    }
    wchar_t *wide = nb_try_alloc((size_t)needed * sizeof(wchar_t));
    if (!wide) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory converting path");
        return NB_ERR_NOMEM;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide,
                            needed) == 0) {
        DWORD code = GetLastError();
        nb_free(wide, (size_t)needed * sizeof(wchar_t));
        if (code == ERROR_NO_UNICODE_TRANSLATION) {
            nb_err_set_os(err, NB_ERR_INVAL, (int)code,
                          "path is not valid UTF-8");
            return NB_ERR_INVAL;
        }
        report_win_error(err, code, "MultiByteToWideChar", text);
        return win_error_status(code);
    }
    *out = wide;
    return NB_OK;
}

static nb_status wide_to_utf8(const wchar_t *wide, char **out, size_t *out_len,
                              nb_err *err)
{
    *out = NULL;
    if (out_len) {
        *out_len = 0;
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) {
        DWORD code = GetLastError();
        report_win_error(err, code, "WideCharToMultiByte", "");
        return win_error_status(code);
    }
    char *utf8 = nb_try_alloc((size_t)needed);
    if (!utf8) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory converting path");
        return NB_ERR_NOMEM;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, needed, NULL, NULL) !=
        needed) {
        DWORD code = GetLastError();
        nb_free(utf8, (size_t)needed);
        report_win_error(err, code, "WideCharToMultiByte", "");
        return win_error_status(code);
    }
    *out = utf8;
    if (out_len) {
        *out_len = (size_t)needed - 1u;
    }
    return NB_OK;
}

static void normalize_separators(char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\\') {
            text[i] = '/';
        }
    }
}

static nb_status write_handle_all(HANDLE handle, const void *data, size_t len,
                                  const char *path, nb_err *err)
{
    const unsigned char *ptr = (const unsigned char *)data;
    while (len > 0) {
        DWORD chunk = (DWORD)(len > (size_t)NB_IO_CHUNK ? NB_IO_CHUNK : len);
        DWORD written = 0;
        if (!WriteFile(handle, ptr, chunk, &written, NULL)) {
            DWORD code = GetLastError();
            report_win_io_error(err, code, "write", path);
            return NB_ERR_IO;
        }
        if (written == 0) {
            nb_err_set(err, NB_ERR_IO, "short write to '%s'", path);
            return NB_ERR_IO;
        }
        ptr += written;
        len -= written;
    }
    return NB_OK;
}

bool nb_file_exists(const char *path)
{
    if (!path) {
        return false;
    }
    wchar_t *wide = NULL;
    if (utf8_to_wide(path, &wide, NULL) != NB_OK) {
        return false;
    }
    DWORD attrs = GetFileAttributesW(wide);
    wide_free(wide);
    return attrs != INVALID_FILE_ATTRIBUTES;
}

bool nb_file_is_dir(const char *path)
{
    if (!path) {
        return false;
    }
    wchar_t *wide = NULL;
    if (utf8_to_wide(path, &wide, NULL) != NB_OK) {
        return false;
    }
    DWORD attrs = GetFileAttributesW(wide);
    wide_free(wide);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

nb_status nb_file_size(const char *path, uint64_t *size, nb_err *err)
{
    if (!path || !size) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or size output");
        return NB_ERR_INVAL;
    }
    wchar_t *wide = NULL;
    nb_status status = utf8_to_wide(path, &wide, err);
    if (status != NB_OK) {
        return status;
    }

    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &info)) {
        DWORD code = GetLastError();
        wide_free(wide);
        report_win_error(err, code, "GetFileAttributesExW", path);
        return win_error_status(code);
    }
    *size = ((uint64_t)info.nFileSizeHigh << 32) |
            (uint64_t)info.nFileSizeLow;
    wide_free(wide);
    return NB_OK;
}

static int64_t filetime_to_unix_ns(FILETIME time)
{
    uint64_t ticks = ((uint64_t)time.dwHighDateTime << 32) |
                     (uint64_t)time.dwLowDateTime;
    const uint64_t epoch = 116444736000000000ULL;
    if (ticks >= epoch) {
        return (int64_t)((ticks - epoch) * 100ULL);
    }
    return -(int64_t)((epoch - ticks) * 100ULL);
}

static nb_dir_entry_type win_entry_type(DWORD attrs, DWORD reparse_tag)
{
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
        (reparse_tag == IO_REPARSE_TAG_SYMLINK ||
         reparse_tag == IO_REPARSE_TAG_MOUNT_POINT)) {
        return NB_DIR_ENTRY_SYMLINK;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return NB_DIR_ENTRY_DIR;
    }
    if ((attrs & FILE_ATTRIBUTE_DEVICE) != 0) {
        return NB_DIR_ENTRY_CHAR;
    }
    return NB_DIR_ENTRY_FILE;
}

nb_status nb_file_stat(const char *path, bool follow_links,
                       nb_file_info *out, nb_err *err)
{
    if (!path || !out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or stat output");
        return NB_ERR_INVAL;
    }
    wchar_t *wide = NULL;
    nb_status status = utf8_to_wide(path, &wide, err);
    if (status != NB_OK) {
        return status;
    }

    uint64_t size = 0;
    int64_t mtime_ns = 0;
    DWORD attrs = 0;
    DWORD reparse_tag = 0;

    if (follow_links) {
        HANDLE handle = CreateFileW(
            wide, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            wide_free(wide);
            report_win_error(err, code, "open", path);
            return win_error_status(code);
        }
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(handle, &info)) {
            DWORD code = GetLastError();
            (void)CloseHandle(handle);
            wide_free(wide);
            report_win_error(err, code, "stat", path);
            return win_error_status(code);
        }
        (void)CloseHandle(handle);
        attrs = info.dwFileAttributes;
        size = ((uint64_t)info.nFileSizeHigh << 32) |
               (uint64_t)info.nFileSizeLow;
        mtime_ns = filetime_to_unix_ns(info.ftLastWriteTime);
    } else {
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW(wide, &data);
        if (find == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            wide_free(wide);
            report_win_error(err, code, "stat", path);
            return win_error_status(code);
        }
        (void)FindClose(find);
        attrs = data.dwFileAttributes;
        reparse_tag = data.dwReserved0;
        size = ((uint64_t)data.nFileSizeHigh << 32) |
               (uint64_t)data.nFileSizeLow;
        mtime_ns = filetime_to_unix_ns(data.ftLastWriteTime);
    }
    wide_free(wide);

    memset(out, 0, sizeof(*out));
    out->size = size;
    out->mtime_ns = mtime_ns;
    out->mode = (attrs & FILE_ATTRIBUTE_READONLY) != 0 ? 0444u : 0644u;
    nb_dir_entry_type type = win_entry_type(attrs, reparse_tag);
    out->is_dir = type == NB_DIR_ENTRY_DIR;
    out->is_symlink = type == NB_DIR_ENTRY_SYMLINK;
    out->is_char_device = type == NB_DIR_ENTRY_CHAR;
    out->is_regular = type == NB_DIR_ENTRY_FILE;
    return NB_OK;
}

nb_status nb_platform_file_read_all(const char *path, nb_buf *out, size_t max_bytes, nb_err *err)
{
    if (!path || !out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or output buffer");
        return NB_ERR_INVAL;
    }
    nb_buf_clear(out);

    wchar_t *wide = NULL;
    nb_status status = utf8_to_wide(path, &wide, err);
    if (status != NB_OK) {
        return status;
    }
    HANDLE handle = CreateFileW(
        wide, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        wide_free(wide);
        report_win_error(err, code, "open", path);
        return win_error_status(code);
    }

    LARGE_INTEGER total;
    if (GetFileSizeEx(handle, &total) && total.QuadPart > 0) {
        uint64_t total_size = (uint64_t)total.QuadPart;
        if (max_bytes && total_size > max_bytes) {
            (void)CloseHandle(handle);
            wide_free(wide);
            nb_err_set(err, NB_ERR_TOO_BIG, "file '%s' exceeds the read limit", path);
            return NB_ERR_TOO_BIG;
        }
        size_t reserve_size = (size_t)total_size;
        if ((uint64_t)reserve_size != total_size) {
            (void)CloseHandle(handle);
            wide_free(wide);
            nb_err_set(err, NB_ERR_RANGE, "file '%s' is too large", path);
            return NB_ERR_RANGE;
        }
        if (!nb_buf_reserve(out, reserve_size)) {
            (void)CloseHandle(handle);
            wide_free(wide);
            nb_err_set(err, NB_ERR_NOMEM, "out of memory reading '%s'", path);
            return NB_ERR_NOMEM;
        }
    }

    unsigned char chunk[NB_IO_CHUNK];
    for (;;) {
        DWORD got = 0;
        size_t amount = sizeof(chunk);
        if (max_bytes && max_bytes - out->len < amount) {
            amount = max_bytes - out->len + 1u;
        }
        if (!ReadFile(handle, chunk, (DWORD)amount, &got, NULL)) {
            DWORD code = GetLastError();
            (void)CloseHandle(handle);
            wide_free(wide);
            report_win_error(err, code, "read", path);
            return win_error_status(code);
        }
        if (got == 0) {
            break;
        }
        if (max_bytes && (size_t)got > max_bytes - out->len) {
            (void)CloseHandle(handle);
            wide_free(wide);
            nb_err_set(err, NB_ERR_TOO_BIG, "file '%s' exceeds the read limit", path);
            return NB_ERR_TOO_BIG;
        }
        if (!nb_buf_append(out, chunk, (size_t)got)) {
            (void)CloseHandle(handle);
            wide_free(wide);
            nb_err_set(err, NB_ERR_NOMEM, "out of memory reading '%s'", path);
            return NB_ERR_NOMEM;
        }
    }

    if (!CloseHandle(handle)) {
        DWORD code = GetLastError();
        wide_free(wide);
        report_win_error(err, code, "close", path);
        return win_error_status(code);
    }
    wide_free(wide);
    return NB_OK;
}

nb_status nb_file_write_all(const char *path, const void *data, size_t len,
                            nb_err *err)
{
    if (!path || (!data && len != 0)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or data");
        return NB_ERR_INVAL;
    }
    wchar_t *wide = NULL;
    nb_status status = utf8_to_wide(path, &wide, err);
    if (status != NB_OK) {
        return status;
    }

    HANDLE handle = CreateFileW(
        wide, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        wide_free(wide);
        report_win_error(err, code, "open", path);
        return win_error_status(code);
    }

    status = write_handle_all(handle, data, len, path, err);
    if (!CloseHandle(handle) && status == NB_OK) {
        DWORD code = GetLastError();
        report_win_error(err, code, "close", path);
        status = win_error_status(code);
    }
    wide_free(wide);
    return status;
}

static nb_status build_temp_path(nb_buf *out, const char *path, unsigned attempt,
                                 const char *prefix)
{
    size_t len = strlen(path);
    size_t dir_len = 0;
    for (size_t i = len; i > 0; i--) {
        if (nb_platform_path_is_sep(path[i - 1u])) {
            dir_len = i;
            break;
        }
    }

    static volatile LONG sequence;
    LONG seq = InterlockedIncrement(&sequence);
    nb_buf_clear(out);
    if (!nb_buf_append(out, path, dir_len) ||
        !nb_buf_append_cstr(out, prefix ? prefix : ".nbase-") ||
        !nb_buf_append_u64(out, (uint64_t)GetCurrentProcessId()) || !nb_buf_append_char(out, '-') ||
        !nb_buf_append_u64(out, (uint64_t)GetTickCount()) || !nb_buf_append_char(out, '-') ||
        !nb_buf_append_i64(out, (int64_t)seq) || !nb_buf_append_char(out, '-') ||
        !nb_buf_append_u64(out, (uint64_t)attempt)) {
        return NB_ERR_NOMEM;
    }
    return NB_OK;
}

static nb_status temp_error_status(DWORD code)
{
    switch (code) {
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return NB_ERR_PERMISSION;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return NB_ERR_NO_SPACE;
    case ERROR_TOO_MANY_OPEN_FILES:
        return NB_ERR_RESOURCE;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return NB_ERR_NOMEM;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return NB_ERR_BUSY;
    default:
        return win_error_status(code);
    }
}

static nb_status create_temp_file(const char *path, DWORD access, DWORD sharing,
                                  bool portable_errors, nb_buf *temp, HANDLE *handle_out,
                                  wchar_t **wide_out, nb_err *err)
{
    for (unsigned attempt = 0; attempt < NB_TEMP_ATTEMPTS; attempt++) {
        nb_status status = build_temp_path(temp, path, attempt, NULL);
        if (status != NB_OK) {
            nb_err_set(err, NB_ERR_NOMEM,
                       "out of memory building temporary path");
            return status;
        }
        wchar_t *wide = NULL;
        status = utf8_to_wide(nb_buf_cstr(temp), &wide, err);
        if (status != NB_OK) {
            return status;
        }
        HANDLE handle =
            CreateFileW(wide, access, sharing, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle != INVALID_HANDLE_VALUE) {
            *handle_out = handle;
            *wide_out = wide;
            return NB_OK;
        }
        DWORD code = GetLastError();
        wide_free(wide);
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
            continue;
        }
        if (portable_errors) {
            status = temp_error_status(code);
            nb_err_set_os(err, status, (int)code, "create temporary file '%s'", nb_buf_cstr(temp));
        } else {
            report_win_io_error(err, code, "create temporary file for", nb_buf_cstr(temp));
            status = NB_ERR_IO;
        }
        return status;
    }
    nb_err_set(err, NB_ERR_EXISTS,
               "could not create a temporary file for '%s'", path);
    return NB_ERR_EXISTS;
}

static nb_status create_temp_file_in(const char *dir, DWORD access, DWORD sharing, nb_buf *temp,
                                     HANDLE *handle_out, wchar_t **wide_out, nb_err *err)
{
    nb_buf *base = nb_buf_new();
    if (!base) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory building temporary path");
        return NB_ERR_NOMEM;
    }
    size_t len = strlen(dir);
    bool ok = nb_buf_append_cstr(base, dir);
    if (ok && (len == 0 || !nb_platform_path_is_sep(dir[len - 1u]))) {
        ok = nb_buf_append_char(base, '/');
    }
    if (!ok) {
        nb_buf_free(base);
        nb_err_set(err, NB_ERR_NOMEM, "out of memory building temporary path");
        return NB_ERR_NOMEM;
    }
    nb_status status =
        create_temp_file(nb_buf_cstr(base), access, sharing, true, temp, handle_out, wide_out, err);
    nb_buf_free(base);
    return status;
}

typedef nb_status (*win_atomic_writer)(HANDLE target, void *context, const char *path, nb_err *err);

static nb_status win_publish_atomic(const char *path, const nb_file_write_options *options,
                                    win_atomic_writer writer, void *context, nb_err *err)
{
    if (!path || path[0] == '\0' || !writer) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or data");
        return NB_ERR_INVAL;
    }
    if (options && options->published) {
        *options->published = false;
    }

    /* NULL keeps the documented safe defaults: durable publish, no follow. */
    bool create_only = options != NULL && options->create_only;
    bool sync_dir = options == NULL || options->sync_dir;
    bool follow_symlink = options != NULL && options->follow_symlink;
    uint32_t requested_mode = options ? options->mode & 07777u : 0u;

    wchar_t *wide_path = NULL;
    nb_status status = utf8_to_wide(path, &wide_path, err);
    if (status != NB_OK) {
        return status;
    }

    DWORD target_attrs = GetFileAttributesW(wide_path);
    bool exists = target_attrs != INVALID_FILE_ATTRIBUTES;
    if (exists && create_only) {
        wide_free(wide_path);
        nb_err_set(err, NB_ERR_EXISTS, "path '%s' already exists", path);
        return NB_ERR_EXISTS;
    }
    if (exists && !follow_symlink) {
        DWORD code = 0;
        if (target_attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            code = ERROR_ACCESS_DENIED;
        } else if (target_attrs & FILE_ATTRIBUTE_DIRECTORY) {
            code = ERROR_DIRECTORY;
        } else if (target_attrs & FILE_ATTRIBUTE_DEVICE) {
            code = ERROR_INVALID_PARAMETER;
        }
        if (code != 0) {
            wide_free(wide_path);
            nb_err_set_os(err, NB_ERR_IO, (int)code,
                          "refusing to replace non-regular path '%s'", path);
            return NB_ERR_IO;
        }
    }

    nb_buf temp;
    nb_buf_init(&temp);
    HANDLE handle = INVALID_HANDLE_VALUE;
    wchar_t *wide_temp = NULL;
    status = create_temp_file(path, GENERIC_WRITE, 0, false, &temp, &handle, &wide_temp, err);

    if (status == NB_OK) {
        status = writer(handle, context, nb_buf_cstr(&temp), err);
        if (status == NB_OK && !FlushFileBuffers(handle)) {
            DWORD code = GetLastError();
            report_win_io_error(err, code, "flush", nb_buf_cstr(&temp));
            status = NB_ERR_IO;
        }
        if (!CloseHandle(handle) && status == NB_OK) {
            DWORD code = GetLastError();
            report_win_io_error(err, code, "close", nb_buf_cstr(&temp));
            status = NB_ERR_IO;
        }
        if (status == NB_OK) {
            bool readonly = false;
            if (!exists && requested_mode == 0 && options && options->new_file_mode != 0) {
                requested_mode = options->new_file_mode & 07777u;
            }
            if (requested_mode != 0) {
                readonly = (requested_mode & 0222u) == 0;
            } else if (exists) {
                readonly = (target_attrs & FILE_ATTRIBUTE_READONLY) != 0;
            }
            if (!SetFileAttributesW(
                    wide_temp, readonly ? FILE_ATTRIBUTE_READONLY
                                        : FILE_ATTRIBUTE_NORMAL)) {
                DWORD code = GetLastError();
                report_win_io_error(err, code, "set attributes on",
                                    nb_buf_cstr(&temp));
                status = NB_ERR_IO;
            }
        }
        if (status == NB_OK) {
            DWORD flags =
                MOVEFILE_WRITE_THROUGH |
                (create_only ? 0u : MOVEFILE_REPLACE_EXISTING);
            if (!MoveFileExW(wide_temp, wide_path, flags)) {
                DWORD code = GetLastError();
                if (create_only &&
                    (code == ERROR_ALREADY_EXISTS ||
                     code == ERROR_FILE_EXISTS)) {
                    nb_err_set(err, NB_ERR_EXISTS,
                               "path '%s' already exists", path);
                    status = NB_ERR_EXISTS;
                } else {
                    report_win_io_error(err, code, "replace", path);
                    status = NB_ERR_IO;
                }
            } else if (options && options->published) {
                *options->published = true;
            }
        }
        if (status != NB_OK) {
            (void)DeleteFileW(wide_temp);
        }
    }

    wide_free(wide_temp);
    nb_buf_destroy(&temp);
    wide_free(wide_path);
    if (status != NB_OK || !sync_dir) {
        return status;
    }

    nb_buf parent;
    nb_buf_init(&parent);
    status = nb_path_dirname(&parent, path);
    if (status == NB_OK) {
        status = nb_file_sync_dir(nb_buf_cstr(&parent), err);
    } else {
        nb_err_set(err, NB_ERR_NOMEM,
                   "out of memory resolving parent of '%s'", path);
    }
    nb_buf_destroy(&parent);
    return status;
}

typedef struct win_atomic_bytes {
    const void *data;
    size_t len;
} win_atomic_bytes;

static nb_status win_write_atomic_bytes(HANDLE target, void *context, const char *path, nb_err *err)
{
    win_atomic_bytes *bytes = context;
    return write_handle_all(target, bytes->data, bytes->len, path, err);
}

nb_status nb_file_write_all_atomic(const char *path, const void *data, size_t len,
                                   const nb_file_write_options *options, nb_err *err)
{
    if (!data && len) {
        nb_err_set(err, NB_ERR_INVAL, "invalid atomic write data");
        return NB_ERR_INVAL;
    }
    win_atomic_bytes bytes = {data, len};
    return win_publish_atomic(path, options, win_write_atomic_bytes, &bytes, err);
}

nb_status nb_file_append(const char *path, const void *data, size_t len,
                         nb_err *err)
{
    if (!path || (!data && len != 0)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or data");
        return NB_ERR_INVAL;
    }
    wchar_t *wide = NULL;
    nb_status status = utf8_to_wide(path, &wide, err);
    if (status != NB_OK) {
        return status;
    }

    HANDLE handle = CreateFileW(
        wide, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        wide_free(wide);
        report_win_io_error(err, code, "open", path);
        return NB_ERR_IO;
    }

    status = write_handle_all(handle, data, len, path, err);
    if (!CloseHandle(handle) && status == NB_OK) {
        DWORD code = GetLastError();
        report_win_io_error(err, code, "close", path);
        status = NB_ERR_IO;
    }
    wide_free(wide);
    return status;
}

nb_status nb_file_remove(const char *path, nb_err *err)
{
    if (!path) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NB_ERR_INVAL;
    }
    wchar_t *wide = NULL;
    nb_status status = utf8_to_wide(path, &wide, err);
    if (status != NB_OK) {
        return status;
    }
    if (!DeleteFileW(wide)) {
        DWORD code = GetLastError();
        wide_free(wide);
        report_win_error(err, code, "remove", path);
        return win_error_status(code);
    }
    wide_free(wide);
    return NB_OK;
}

nb_status nb_file_rename(const char *from, const char *to, nb_err *err)
{
    if (!from || !to) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NB_ERR_INVAL;
    }
    wchar_t *wide_from = NULL;
    nb_status status = utf8_to_wide(from, &wide_from, err);
    if (status != NB_OK) {
        return status;
    }
    wchar_t *wide_to = NULL;
    status = utf8_to_wide(to, &wide_to, err);
    if (status != NB_OK) {
        wide_free(wide_from);
        return status;
    }
    if (!MoveFileExW(wide_from, wide_to, MOVEFILE_REPLACE_EXISTING)) {
        DWORD code = GetLastError();
        wide_free(wide_to);
        wide_free(wide_from);
        report_win_io_error(err, code, "rename", from);
        return NB_ERR_IO;
    }
    wide_free(wide_to);
    wide_free(wide_from);
    return NB_OK;
}

nb_status nb_file_rename_noreplace(const char *from, const char *to,
                                   nb_err *err)
{
    if (!from || !to) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NB_ERR_INVAL;
    }
    wchar_t *wide_from = NULL;
    nb_status status = utf8_to_wide(from, &wide_from, err);
    if (status != NB_OK) {
        return status;
    }
    wchar_t *wide_to = NULL;
    status = utf8_to_wide(to, &wide_to, err);
    if (status != NB_OK) {
        wide_free(wide_from);
        return status;
    }
    if (!MoveFileExW(wide_from, wide_to, MOVEFILE_WRITE_THROUGH)) {
        DWORD code = GetLastError();
        wide_free(wide_to);
        wide_free(wide_from);
        if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
            nb_err_set(err, NB_ERR_EXISTS, "path '%s' already exists", to);
            return NB_ERR_EXISTS;
        }
        report_win_io_error(err, code, "rename", from);
        return NB_ERR_IO;
    }
    wide_free(wide_to);
    wide_free(wide_from);
    return NB_OK;
}

static nb_status create_dir_one(const wchar_t *wide, const char *path,
                                nb_err *err)
{
    if (CreateDirectoryW(wide, NULL)) {
        return NB_OK;
    }
    DWORD code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
        DWORD attrs = GetFileAttributesW(wide);
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return NB_OK;
        }
        nb_err_set(err, NB_ERR_EXISTS,
                   "path '%s' exists and is not a directory", path);
        return NB_ERR_EXISTS;
    }
    report_win_error(err, code, "mkdir", path);
    return win_error_status(code);
}

/* Length of the immutable root prefix, so recursive creation never tries to
 * create a drive, a UNC server, or an extended-path root. */
static size_t wide_root_len(const wchar_t *path, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (len >= 2u && win_is_sep(path[0]) && win_is_sep(path[1])) {
        size_t i = 2u;
        bool extended =
            len >= i + 2u && (path[i] == L'?' || path[i] == L'.') &&
            win_is_sep(path[i + 1u]);
        if (extended) {
            i += 2u;
            if (len >= i + 4u && win_upper(path[i]) == L'U' &&
                win_upper(path[i + 1u]) == L'N' &&
                win_upper(path[i + 2u]) == L'C' &&
                win_is_sep(path[i + 3u])) {
                i += 4u;
                while (i < len && !win_is_sep(path[i])) {
                    i++;
                }
                if (i < len) {
                    i++;
                }
                while (i < len && !win_is_sep(path[i])) {
                    i++;
                }
                return i;
            }
            if (len >= i + 2u && win_is_alpha(path[i]) &&
                path[i + 1u] == L':') {
                i += 2u;
                if (i < len && win_is_sep(path[i])) {
                    i++;
                }
                return i;
            }
            return i;
        }
        while (i < len && !win_is_sep(path[i])) {
            i++;
        }
        if (i < len) {
            i++;
        }
        while (i < len && !win_is_sep(path[i])) {
            i++;
        }
        return i;
    }
    if (len >= 2u && win_is_alpha(path[0]) && path[1] == L':') {
        if (len >= 3u && win_is_sep(path[2])) {
            return 3u;
        }
        return 2u;
    }
    if (win_is_sep(path[0])) {
        return 1u;
    }
    return 0u;
}

static nb_status mkdir_recursive(const wchar_t *wide, const char *path,
                                 nb_err *err)
{
    size_t wide_len = wcslen(wide);
    size_t bytes = (wide_len + 1u) * sizeof(wchar_t);
    wchar_t *work = nb_try_alloc(bytes);
    if (!work) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory creating directory");
        return NB_ERR_NOMEM;
    }
    memcpy(work, wide, bytes);

    size_t len = wide_len;
    size_t root = wide_root_len(work, len);
    while (len > root && win_is_sep(work[len - 1u])) {
        len--;
    }

    nb_status status = NB_OK;
    size_t i = root;
    while (i < len && status == NB_OK) {
        while (i < len && win_is_sep(work[i])) {
            i++;
        }
        if (i >= len) {
            break;
        }
        while (i < len && !win_is_sep(work[i])) {
            i++;
        }
        wchar_t saved = work[i];
        work[i] = L'\0';
        status = create_dir_one(work, path, err);
        work[i] = saved;
    }

    if (status == NB_OK) {
        wchar_t saved = work[len];
        work[len] = L'\0';
        DWORD attrs = GetFileAttributesW(work);
        DWORD code =
            attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        work[len] = saved;
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            report_win_error(err, code, "mkdir", path);
            status = win_error_status(code);
        } else if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            nb_err_set(err, NB_ERR_IO, "path '%s' is not a directory", path);
            status = NB_ERR_IO;
        }
    }

    nb_free(work, bytes);
    return status;
}

nb_status nb_file_mkdir(const char *path, bool recursive, nb_err *err)
{
    if (!path || path[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NB_ERR_INVAL;
    }
    wchar_t *wide = NULL;
    nb_status status = utf8_to_wide(path, &wide, err);
    if (status != NB_OK) {
        return status;
    }
    status = recursive ? mkdir_recursive(wide, path, err)
                       : create_dir_one(wide, path, err);
    wide_free(wide);
    return status;
}

nb_status nb_file_rmdir(const char *path, nb_err *err)
{
    if (!path) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NB_ERR_INVAL;
    }
    wchar_t *wide = NULL;
    nb_status status = utf8_to_wide(path, &wide, err);
    if (status != NB_OK) {
        return status;
    }
    if (!RemoveDirectoryW(wide)) {
        DWORD code = GetLastError();
        wide_free(wide);
        report_win_error(err, code, "rmdir", path);
        return win_error_status(code);
    }
    wide_free(wide);
    return NB_OK;
}

static nb_status get_temp_path(wchar_t **out, size_t *out_bytes, nb_err *err)
{
    size_t cap = (size_t)MAX_PATH + 1u;
    wchar_t *wide = nb_try_alloc(cap * sizeof(wchar_t));
    if (!wide) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory resolving temp path");
        return NB_ERR_NOMEM;
    }
    for (;;) {
        DWORD needed = GetTempPathW((DWORD)cap, wide);
        if (needed == 0) {
            DWORD code = GetLastError();
            nb_free(wide, cap * sizeof(wchar_t));
            report_win_error(err, code, "GetTempPathW", "");
            return win_error_status(code);
        }
        if ((size_t)needed < cap) {
            *out = wide;
            *out_bytes = cap * sizeof(wchar_t);
            return NB_OK;
        }
        size_t new_cap = (size_t)needed;
        if (new_cap <= cap) {
            new_cap = cap + 1u;
        }
        wchar_t *grown = nb_try_alloc(new_cap * sizeof(wchar_t));
        if (!grown) {
            nb_free(wide, cap * sizeof(wchar_t));
            nb_err_set(err, NB_ERR_NOMEM,
                       "out of memory resolving temp path");
            return NB_ERR_NOMEM;
        }
        nb_free(wide, cap * sizeof(wchar_t));
        wide = grown;
        cap = new_cap;
    }
}

nb_status nb_file_temp(nb_buf *path_out, nb_err *err)
{
    if (!path_out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid output buffer");
        return NB_ERR_INVAL;
    }
    wchar_t *temp_dir = NULL;
    size_t temp_dir_bytes = 0;
    nb_status status = get_temp_path(&temp_dir, &temp_dir_bytes, err);
    if (status != NB_OK) {
        return status;
    }
    char *utf8 = NULL;
    size_t len = 0;
    status = wide_to_utf8(temp_dir, &utf8, &len, err);
    nb_free(temp_dir, temp_dir_bytes);
    if (status != NB_OK) {
        return status;
    }
    normalize_separators(utf8, len);

    nb_buf dir = {.ptr = utf8, .len = len, .cap = len + 1u};
    status = nb_file_temp_in(path_out, nb_buf_cstr(&dir), NULL, err);
    nb_buf_destroy(&dir);
    return status;
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
    nb_buf temp;
    nb_buf_init(&temp);
    HANDLE handle = INVALID_HANDLE_VALUE;
    wchar_t *wide_temp = NULL;
    DWORD access = fd_out ? GENERIC_READ | GENERIC_WRITE : GENERIC_WRITE;
    DWORD sharing = fd_out ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE : 0;
    nb_status status = create_temp_file_in(dir, access, sharing, &temp, &handle, &wide_temp, err);
    if (status == NB_OK) {
        if (fd_out) {
            int fd = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_RDWR | _O_NOINHERIT);
            if (fd < 0) {
                status = nb_status_from_errno(errno, err);
                (void)CloseHandle(handle);
                (void)DeleteFileW(wide_temp);
            } else {
                *fd_out = fd;
            }
        } else if (!CloseHandle(handle)) {
            DWORD code = GetLastError();
            status = temp_error_status(code);
            nb_err_set_os(err, status, (int)code, "close temporary file '%s'", nb_buf_cstr(&temp));
            (void)DeleteFileW(wide_temp);
        }
    }
    wide_free(wide_temp);
    if (status == NB_OK) {
        nb_buf_move(path_out, &temp);
    }
    nb_buf_destroy(&temp);
    return status;
}

nb_status nb_file_temp_dir(nb_buf *path_out, const char *parent, const char *prefix, nb_err *err)
{
    if (!path_out || (prefix && (strchr(prefix, '/') || strchr(prefix, '\\')))) {
        nb_err_set(err, NB_ERR_INVAL, "invalid output buffer");
        return NB_ERR_INVAL;
    }
    nb_buf dir = {0};
    nb_status status;
    if (parent) {
        if (!*parent) {
            nb_err_set(err, NB_ERR_INVAL, "invalid parent directory");
            return NB_ERR_INVAL;
        }
        if (!nb_buf_append_cstr(&dir, parent)) {
            nb_buf_destroy(&dir);
            nb_err_set(err, NB_ERR_NOMEM, "out of memory copying parent directory");
            return NB_ERR_NOMEM;
        }
    } else {
        wchar_t *temp_dir = NULL;
        size_t temp_dir_bytes = 0;
        status = get_temp_path(&temp_dir, &temp_dir_bytes, err);
        if (status != NB_OK)
            return status;
        char *utf8 = NULL;
        size_t len = 0;
        status = wide_to_utf8(temp_dir, &utf8, &len, err);
        nb_free(temp_dir, temp_dir_bytes);
        if (status != NB_OK)
            return status;
        normalize_separators(utf8, len);
        dir = (nb_buf){.ptr = utf8, .len = len, .cap = len + 1u};
    }

    nb_buf *base = nb_buf_new();
    if (!base) {
        nb_buf_destroy(&dir);
        nb_err_set(err, NB_ERR_NOMEM, "out of memory creating temporary path");
        return NB_ERR_NOMEM;
    }
    bool ok = nb_buf_append(base, nb_buf_cstr(&dir), nb_buf_len(&dir));
    if (ok && (nb_buf_len(&dir) == 0 ||
               !nb_platform_path_is_sep(nb_buf_cstr(&dir)[nb_buf_len(&dir) - 1u]))) {
        ok = nb_buf_append_char(base, '/');
    }
    nb_buf_destroy(&dir);
    if (!ok) {
        nb_buf_free(base);
        nb_err_set(err, NB_ERR_NOMEM, "out of memory creating temporary path");
        return NB_ERR_NOMEM;
    }

    nb_buf temp;
    nb_buf_init(&temp);
    status = NB_ERR_EXISTS;
    for (unsigned attempt = 0; attempt < NB_TEMP_ATTEMPTS; attempt++) {
        status = build_temp_path(&temp, nb_buf_cstr(base), attempt, prefix ? prefix : "nbase-");
        if (status != NB_OK) {
            nb_err_set(err, NB_ERR_NOMEM,
                       "out of memory building temporary path");
            break;
        }
        wchar_t *wide = NULL;
        status = utf8_to_wide(nb_buf_cstr(&temp), &wide, err);
        if (status != NB_OK) {
            break;
        }
        if (CreateDirectoryW(wide, NULL)) {
            wide_free(wide);
            nb_buf_move(path_out, &temp);
            status = NB_OK;
            break;
        }
        DWORD code = GetLastError();
        wide_free(wide);
        if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
            status = NB_ERR_EXISTS;
            continue;
        }
        status = temp_error_status(code);
        nb_err_set_os(err, status, (int)code, "create temporary directory '%s'",
                      nb_buf_cstr(&temp));
        break;
    }
    if (status == NB_ERR_EXISTS) {
        nb_err_set(err, NB_ERR_EXISTS,
                   "could not create a temporary directory in '%s'",
                   nb_buf_cstr(base));
    }
    nb_buf_destroy(&temp);
    nb_buf_free(base);
    return status;
}

nb_status nb_file_sync_dir(const char *dir, nb_err *err)
{
    if (!dir || dir[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid directory");
        return NB_ERR_INVAL;
    }
    wchar_t *wide = NULL;
    nb_status status = utf8_to_wide(dir, &wide, err);
    if (status != NB_OK) {
        return status;
    }
    HANDLE handle = CreateFileW(
        wide, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        wide_free(wide);
        report_win_error(err, code, "open directory", dir);
        return win_error_status(code);
    }
    wide_free(wide);

    BOOL flushed = FlushFileBuffers(handle);
    DWORD code = flushed ? ERROR_SUCCESS : GetLastError();
    (void)CloseHandle(handle);
    if (flushed) {
        return NB_OK;
    }
    /* Directory flush depends on the volume and handle rights. MoveFileEx with
     * MOVEFILE_WRITE_THROUGH already covers the publish calls this supports. */
    if (code == ERROR_INVALID_FUNCTION || code == ERROR_NOT_SUPPORTED ||
        code == ERROR_ACCESS_DENIED || code == ERROR_INVALID_HANDLE ||
        code == ERROR_INVALID_PARAMETER) {
        return NB_OK;
    }
    report_win_io_error(err, code, "flush directory", dir);
    return NB_ERR_IO;
}

struct nb_file_lock {
    HANDLE handle;
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

    wchar_t *wide = NULL;
    nb_status status = utf8_to_wide(path, &wide, err);
    if (status != NB_OK) {
        return status;
    }
    HANDLE handle = CreateFileW(
        wide, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    wide_free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        report_win_io_error(err, code, "open lock", path);
        return NB_ERR_IO;
    }

    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof(overlapped));
    DWORD flags = (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0u) |
                  (blocking ? 0u : LOCKFILE_FAIL_IMMEDIATELY);
    if (!LockFileEx(handle, flags, 0, MAXDWORD, MAXDWORD, &overlapped)) {
        DWORD code = GetLastError();
        (void)CloseHandle(handle);
        if (code == ERROR_LOCK_VIOLATION || code == ERROR_IO_PENDING) {
            nb_err_set(err, NB_ERR_AGAIN, "lock '%s' is held by another owner",
                       path);
            return NB_ERR_AGAIN;
        }
        report_win_io_error(err, code, "lock", path);
        return NB_ERR_IO;
    }

    nb_file_lock *lock = nb_try_alloc(sizeof(*lock));
    if (!lock) {
        OVERLAPPED unlock;
        memset(&unlock, 0, sizeof(unlock));
        (void)UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &unlock);
        (void)CloseHandle(handle);
        nb_err_set(err, NB_ERR_NOMEM, "out of memory acquiring lock");
        return NB_ERR_NOMEM;
    }
    lock->handle = handle;
    *out = lock;
    return NB_OK;
}

void nb_file_lock_release(nb_file_lock *lock)
{
    if (!lock) {
        return;
    }
    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof(overlapped));
    (void)UnlockFileEx(lock->handle, 0, MAXDWORD, MAXDWORD, &overlapped);
    (void)CloseHandle(lock->handle);
    nb_free(lock, sizeof(*lock));
}

struct nb_dir {
    HANDLE find;
    WIN32_FIND_DATAW data;
    nb_buf name;
    nb_status status;
    bool first;
};

static bool is_dot_entry(const wchar_t *name)
{
    return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0;
}

static nb_status name_to_buf(nb_buf *out, const wchar_t *name)
{
    nb_buf_clear(out);
    int needed = WideCharToMultiByte(CP_UTF8, 0, name, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) {
        return NB_ERR_IO;
    }
    if (!nb_buf_reserve(out, (size_t)needed)) {
        return NB_ERR_NOMEM;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, name, -1, out->ptr, needed, NULL,
                            NULL) != needed) {
        nb_buf_clear(out);
        return NB_ERR_IO;
    }
    out->len = (size_t)needed - 1u;
    out->ptr[out->len] = '\0';
    return NB_OK;
}

nb_dir *nb_dir_open(const char *path, nb_err *err)
{
    if (!path) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NULL;
    }
    if (path[0] == '\0') {
        nb_err_set(err, NB_ERR_NOT_FOUND, "empty path");
        return NULL;
    }

    nb_buf pattern;
    nb_buf_init(&pattern);
    size_t len = strlen(path);
    bool ok = nb_buf_append_cstr(&pattern, path);
    if (ok && !nb_platform_path_is_sep(path[len - 1u])) {
        if (path[len - 1u] == ':') {
            ok = nb_buf_append_char(&pattern, '.');
        }
        if (ok) {
            ok = nb_buf_append_char(&pattern, '\\');
        }
    }
    if (ok) {
        ok = nb_buf_append_char(&pattern, '*');
    }
    if (!ok) {
        nb_buf_destroy(&pattern);
        nb_err_set(err, NB_ERR_NOMEM, "out of memory opening directory");
        return NULL;
    }

    wchar_t *wide_path = NULL;
    wchar_t *wide_pattern = NULL;
    nb_status status = utf8_to_wide(path, &wide_path, err);
    if (status == NB_OK) {
        status = utf8_to_wide(nb_buf_cstr(&pattern), &wide_pattern, err);
    }
    if (status != NB_OK) {
        wide_free(wide_pattern);
        wide_free(wide_path);
        nb_buf_destroy(&pattern);
        return NULL;
    }

    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW(wide_pattern, &found);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) {
            DWORD attrs = GetFileAttributesW(wide_path);
            DWORD attr_code = attrs == INVALID_FILE_ATTRIBUTES
                                  ? GetLastError()
                                  : ERROR_SUCCESS;
            if (attrs != INVALID_FILE_ATTRIBUTES &&
                (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                /* An existing empty directory has no first match. */
            } else if (attrs == INVALID_FILE_ATTRIBUTES) {
                report_win_error(err, attr_code, "open directory", path);
                wide_free(wide_pattern);
                wide_free(wide_path);
                nb_buf_destroy(&pattern);
                return NULL;
            } else {
                nb_err_set(err, NB_ERR_IO, "path '%s' is not a directory",
                           path);
                wide_free(wide_pattern);
                wide_free(wide_path);
                nb_buf_destroy(&pattern);
                return NULL;
            }
        } else {
            report_win_error(err, code, "FindFirstFileW", path);
            wide_free(wide_pattern);
            wide_free(wide_path);
            nb_buf_destroy(&pattern);
            return NULL;
        }
    }

    nb_dir *dir = nb_try_alloc(sizeof(*dir));
    if (!dir) {
        if (find != INVALID_HANDLE_VALUE) {
            (void)FindClose(find);
        }
        wide_free(wide_pattern);
        wide_free(wide_path);
        nb_buf_destroy(&pattern);
        nb_err_set(err, NB_ERR_NOMEM, "out of memory opening directory");
        return NULL;
    }

    dir->find = find;
    dir->first = find != INVALID_HANDLE_VALUE;
    dir->status = NB_OK;
    memset(&dir->data, 0, sizeof(dir->data));
    if (dir->first) {
        dir->data = found;
    }
    nb_buf_init(&dir->name);

    wide_free(wide_pattern);
    wide_free(wide_path);
    nb_buf_destroy(&pattern);
    return dir;
}

const char *nb_dir_next(nb_dir *dir, nb_dir_entry *entry_out)
{
    if (!dir || dir->status != NB_OK) {
        return NULL;
    }
    for (;;) {
        if (dir->first) {
            dir->first = false;
        } else {
            if (dir->find == INVALID_HANDLE_VALUE) {
                return NULL;
            }
            if (!FindNextFileW(dir->find, &dir->data)) {
                DWORD code = GetLastError();
                if (code != ERROR_NO_MORE_FILES) {
                    dir->status = win_error_status(code);
                }
                (void)FindClose(dir->find);
                dir->find = INVALID_HANDLE_VALUE;
                return NULL;
            }
        }
        if (is_dot_entry(dir->data.cFileName)) {
            continue;
        }
        nb_status status = name_to_buf(&dir->name, dir->data.cFileName);
        if (status != NB_OK) {
            dir->status = status;
            return NULL;
        }
        if (entry_out) {
            entry_out->name = nb_buf_cstr(&dir->name);
            entry_out->type =
                win_entry_type(dir->data.dwFileAttributes,
                               dir->data.dwReserved0);
        }
        return nb_buf_cstr(&dir->name);
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
    if (dir->find != INVALID_HANDLE_VALUE) {
        (void)FindClose(dir->find);
    }
    nb_buf_destroy(&dir->name);
    nb_free(dir, sizeof(*dir));
}

static bool win_code_missing(DWORD code)
{
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

nb_status nb_file_set_mode(const char *path, uint32_t mode, nb_err *err)
{
    if (!path || path[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NB_ERR_INVAL;
    }
    wchar_t *wide = NULL;
    nb_status status = utf8_to_wide(path, &wide, err);
    if (status != NB_OK) {
        return status;
    }
    HANDLE handle = CreateFileW(
        wide, FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    wide_free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        report_win_error(err, code, "open", path);
        return win_error_status(code);
    }

    FILE_BASIC_INFO info;
    memset(&info, 0, sizeof(info));
    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &info,
                                      sizeof(info))) {
        DWORD code = GetLastError();
        (void)CloseHandle(handle);
        report_win_error(err, code, "stat", path);
        return win_error_status(code);
    }
    if ((mode & 0222u) != 0) {
        info.FileAttributes &= ~(DWORD)FILE_ATTRIBUTE_READONLY;
    } else {
        info.FileAttributes |= FILE_ATTRIBUTE_READONLY;
    }
    if (!SetFileInformationByHandle(handle, FileBasicInfo, &info,
                                    sizeof(info))) {
        DWORD code = GetLastError();
        (void)CloseHandle(handle);
        report_win_error(err, code, "set mode", path);
        return win_error_status(code);
    }
    (void)CloseHandle(handle);
    return NB_OK;
}

nb_status nb_file_symlink(const char *target, const char *link_path,
                          nb_err *err)
{
    if (!target || !link_path || link_path[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid symbolic link path");
        return NB_ERR_INVAL;
    }
    wchar_t *wide_target = NULL;
    nb_status status = utf8_to_wide(target, &wide_target, err);
    if (status != NB_OK) {
        return status;
    }
    wchar_t *wide_link = NULL;
    status = utf8_to_wide(link_path, &wide_link, err);
    if (status != NB_OK) {
        wide_free(wide_target);
        return status;
    }

    /* Resolve relative targets against the link's directory just for the
     * directory flag; the stored target keeps the caller's spelling. */
    nb_buf *resolved = nb_buf_new();
    if (!resolved) {
        wide_free(wide_link);
        wide_free(wide_target);
        nb_err_set(err, NB_ERR_NOMEM,
                   "out of memory creating symbolic link");
        return NB_ERR_NOMEM;
    }
    if (nb_path_is_absolute(target)) {
        status = nb_buf_append_cstr(resolved, target) ? NB_OK : NB_ERR_NOMEM;
    } else {
        nb_buf *parent = nb_buf_new();
        if (!parent) {
            status = NB_ERR_NOMEM;
        } else {
            status = nb_path_dirname(parent, link_path);
            if (status == NB_OK) {
                status = nb_path_join(resolved, nb_buf_cstr(parent), target);
            }
            nb_buf_free(parent);
        }
    }
    if (status != NB_OK) {
        nb_buf_free(resolved);
        wide_free(wide_link);
        wide_free(wide_target);
        nb_err_set(err, status, "cannot resolve symbolic link target");
        return status;
    }

    wchar_t *wide_resolved = NULL;
    status = utf8_to_wide(nb_buf_cstr(resolved), &wide_resolved, NULL);
    nb_buf_free(resolved);
    bool target_is_dir = false;
    if (status == NB_OK) {
        DWORD attrs = GetFileAttributesW(wide_resolved);
        target_is_dir =
            attrs != INVALID_FILE_ATTRIBUTES &&
            (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        wide_free(wide_resolved);
    } else {
        /* A missing target is still a valid link; only the flag is unknown. */
        status = NB_OK;
    }

    DWORD flags = target_is_dir ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0u;
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2u
#endif
    DWORD unprivileged = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    DWORD code = ERROR_SUCCESS;
    if (!CreateSymbolicLinkW(wide_link, wide_target, flags | unprivileged)) {
        code = GetLastError();
        if (code == ERROR_PRIVILEGE_NOT_HELD ||
            code == ERROR_INVALID_PARAMETER) {
            if (CreateSymbolicLinkW(wide_link, wide_target, flags)) {
                code = ERROR_SUCCESS;
            } else {
                code = GetLastError();
            }
        }
    }

    /* Some providers (notably Wine) can report success without creating the
     * link. Verify the result and retry once with the unprivileged flag. */
    if (code == ERROR_SUCCESS &&
        GetFileAttributesW(wide_link) == INVALID_FILE_ATTRIBUTES) {
        if (CreateSymbolicLinkW(wide_link, wide_target, flags | unprivileged) &&
            GetFileAttributesW(wide_link) != INVALID_FILE_ATTRIBUTES) {
            code = ERROR_SUCCESS;
        } else {
            code = ERROR_NOT_SUPPORTED;
        }
    }
    if (code != ERROR_SUCCESS) {
        wide_free(wide_link);
        wide_free(wide_target);
        if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
            nb_err_set(err, NB_ERR_EXISTS, "path '%s' already exists",
                       link_path);
            return NB_ERR_EXISTS;
        }
        if (code == ERROR_PRIVILEGE_NOT_HELD || code == ERROR_NOT_SUPPORTED ||
            code == ERROR_CALL_NOT_IMPLEMENTED) {
            nb_err_set(err, NB_ERR_UNSUPPORTED,
                       "symbolic link creation is not supported on this "
                       "system");
            return NB_ERR_UNSUPPORTED;
        }
        if (win_code_missing(code)) {
            nb_err_set_os(err, NB_ERR_NOT_FOUND, (int)code,
                          "symlink '%s' failed (win32 %lu)", link_path,
                          (unsigned long)code);
            return NB_ERR_NOT_FOUND;
        }
        report_win_io_error(err, code, "symlink", link_path);
        return NB_ERR_IO;
    }
    wide_free(wide_link);
    wide_free(wide_target);
    return NB_OK;
}

typedef struct nb_win_symlink_reparse {
    ULONG tag;
    USHORT data_length;
    USHORT reserved;
    USHORT substitute_offset;
    USHORT substitute_length;
    USHORT print_offset;
    USHORT print_length;
    ULONG flags;
    WCHAR path_buffer[1];
} nb_win_symlink_reparse;

typedef struct nb_win_mount_reparse {
    ULONG tag;
    USHORT data_length;
    USHORT reserved;
    USHORT substitute_offset;
    USHORT substitute_length;
    USHORT print_offset;
    USHORT print_length;
    WCHAR path_buffer[1];
} nb_win_mount_reparse;

static nb_status win_reparse_target(const wchar_t *name, size_t chars,
                                    nb_buf *out, nb_err *err)
{
    while (chars >= 4u && name[0] == L'\\' && name[1] == L'?' &&
           name[2] == L'?' && name[3] == L'\\') {
        name += 4;
        chars -= 4u;
    }
    if (chars > (size_t)INT_MAX) {
        nb_err_set(err, NB_ERR_RANGE, "symbolic link target is too long");
        return NB_ERR_RANGE;
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, name, (int)chars, NULL, 0,
                                     NULL, NULL);
    if (needed <= 0) {
        DWORD code = GetLastError();
        report_win_io_error(err, code, "convert symbolic link target", "");
        return NB_ERR_IO;
    }
    char *utf8 = nb_try_alloc((size_t)needed);
    if (!utf8) {
        nb_err_set(err, NB_ERR_NOMEM,
                   "out of memory reading symbolic link target");
        return NB_ERR_NOMEM;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, name, (int)chars, utf8, needed, NULL,
                            NULL) != needed) {
        DWORD code = GetLastError();
        nb_free(utf8, (size_t)needed);
        report_win_io_error(err, code, "convert symbolic link target", "");
        return NB_ERR_IO;
    }
    nb_buf_clear(out);
    bool ok = nb_buf_reserve(out, (size_t)needed);
    for (int i = 0; ok && i < needed; i++) {
        ok = nb_buf_append_char(out, utf8[i] == '\\' ? '/' : utf8[i]);
    }
    nb_free(utf8, (size_t)needed);
    if (!ok) {
        nb_err_set(err, NB_ERR_NOMEM,
                   "out of memory reading symbolic link target");
        return NB_ERR_NOMEM;
    }
    return NB_OK;
}

nb_status nb_file_readlink(nb_buf *out, const char *path, nb_err *err)
{
    if (!out || !path || path[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or output buffer");
        return NB_ERR_INVAL;
    }
    wchar_t *wide = NULL;
    nb_status status = utf8_to_wide(path, &wide, err);
    if (status != NB_OK) {
        return status;
    }
    HANDLE handle = CreateFileW(
        wide, FILE_READ_ATTRIBUTES | FILE_READ_EA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    wide_free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        report_win_error(err, code, "open", path);
        return win_error_status(code);
    }

#ifndef MAXIMUM_REPARSE_DATA_BUFFER_SIZE
#define MAXIMUM_REPARSE_DATA_BUFFER_SIZE 16384
#endif
    size_t capacity = (size_t)MAXIMUM_REPARSE_DATA_BUFFER_SIZE + 64u;
    unsigned char *buffer = nb_try_alloc(capacity);
    if (!buffer) {
        (void)CloseHandle(handle);
        nb_err_set(err, NB_ERR_NOMEM,
                   "out of memory reading symbolic link '%s'", path);
        return NB_ERR_NOMEM;
    }
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, NULL, 0,
                              buffer, (DWORD)capacity, &returned, NULL);
    DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    (void)CloseHandle(handle);
    if (!ok) {
        nb_free(buffer, capacity);
        nb_status mapped =
            code == ERROR_NOT_A_REPARSE_POINT
                ? NB_ERR_INVAL
                : (win_code_missing(code) ? NB_ERR_NOT_FOUND : NB_ERR_IO);
        nb_err_set_os(err, mapped, (int)code,
                      "readlink '%s' failed (win32 %lu)", path,
                      (unsigned long)code);
        return mapped;
    }

    const unsigned char *start = buffer;
    const unsigned char *end = start + returned;
    const wchar_t *name = NULL;
    size_t chars = 0;
    ULONG tag = returned >= sizeof(ULONG) ? *(const ULONG *)buffer : 0u;
    if (tag == IO_REPARSE_TAG_SYMLINK &&
        returned >= sizeof(nb_win_symlink_reparse) - sizeof(WCHAR)) {
        const nb_win_symlink_reparse *link =
            (const nb_win_symlink_reparse *)buffer;
        const unsigned char *base = (const unsigned char *)link->path_buffer;
        size_t offset = link->substitute_offset;
        size_t length = link->substitute_length;
        if (base <= end && offset <= (size_t)(end - base) &&
            length <= (size_t)(end - base) - offset &&
            (length % sizeof(WCHAR)) == 0u) {
            name = (const wchar_t *)(base + offset);
            chars = length / sizeof(WCHAR);
        }
    } else if (tag == IO_REPARSE_TAG_MOUNT_POINT &&
               returned >= sizeof(nb_win_mount_reparse) - sizeof(WCHAR)) {
        const nb_win_mount_reparse *mount =
            (const nb_win_mount_reparse *)buffer;
        const unsigned char *base = (const unsigned char *)mount->path_buffer;
        size_t offset = mount->substitute_offset;
        size_t length = mount->substitute_length;
        if (base <= end && offset <= (size_t)(end - base) &&
            length <= (size_t)(end - base) - offset &&
            (length % sizeof(WCHAR)) == 0u) {
            name = (const wchar_t *)(base + offset);
            chars = length / sizeof(WCHAR);
        }
    }
    if (!name) {
        nb_free(buffer, capacity);
        nb_err_set(err, NB_ERR_INVAL, "path '%s' is not a symbolic link",
                   path);
        return NB_ERR_INVAL;
    }

    nb_buf temporary;
    nb_buf_init(&temporary);
    status = win_reparse_target(name, chars, &temporary, err);
    nb_free(buffer, capacity);
    if (status == NB_OK) {
        nb_buf_move(out, &temporary);
    }
    nb_buf_destroy(&temporary);
    return status;
}

static void win_copy_handle_cleanup(HANDLE *handle)
{
    if (*handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(*handle);
        *handle = INVALID_HANDLE_VALUE;
    }
}

static void win_copy_path_cleanup(wchar_t **path)
{
    wide_free(*path);
    *path = NULL;
}

static nb_status win_copy_file_bytes(HANDLE target, void *context, const char *path, nb_err *err)
{
    HANDLE source = *(HANDLE *)context;
    unsigned char chunk[NB_IO_CHUNK];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(source, chunk, (DWORD)sizeof(chunk), &got, NULL)) {
            DWORD code = GetLastError();
            report_win_error(err, code, "read copy source for", path);
            return win_error_status(code);
        }
        if (!got) {
            return NB_OK;
        }
        nb_status status = write_handle_all(target, chunk, (size_t)got, path, err);
        if (status != NB_OK) {
            return status;
        }
    }
}

static nb_status win_copy_opened_file(const char *src, const char *dst, const wchar_t *wide_src,
                                      const wchar_t *wide_dst, const nb_file_copy_options *options,
                                      uint32_t source_mode, HANDLE *source_out, nb_err *err)
{
    DWORD attrs = GetFileAttributesW(wide_dst);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        bool reparse = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (!reparse && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            nb_err_set(err, NB_ERR_EXISTS, "destination '%s' is a directory", dst);
            return NB_ERR_EXISTS;
        }
        if (!options->overwrite) {
            nb_err_set(err, NB_ERR_EXISTS, "path '%s' already exists", dst);
            return NB_ERR_EXISTS;
        }
    }
    HANDLE source = CreateFileW(
        wide_src, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN | (options->nofollow_source ? FILE_FLAG_OPEN_REPARSE_POINT : 0),
        NULL);
    *source_out = source;
    if (source == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        report_win_error(err, code, "open copy source", src);
        return win_error_status(code);
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(source, &info)) {
        DWORD code = GetLastError();
        report_win_error(err, code, "inspect copy source", src);
        return win_error_status(code);
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) ||
        (options->nofollow_source && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))) {
        nb_err_set(err, NB_ERR_PERMISSION, "copy source is not an allowed regular file");
        return NB_ERR_PERMISSION;
    }
    nb_file_write_options write_options = {0};
    write_options.mode = options->preserve_mode ? (source_mode & 07777u) : 0u;
    write_options.new_file_mode = options->new_file_mode;
    write_options.create_only = !options->overwrite;
    write_options.sync_dir = true;
    write_options.follow_symlink = true;
    return win_publish_atomic(dst, &write_options, win_copy_file_bytes, &source, err);
}

static nb_status win_copy_regular_file(
    const char *src, const char *dst, const nb_file_copy_options *options,
    uint32_t source_mode, nb_err *err)
{
    wchar_t *wide_src = NULL;
    wchar_t *wide_dst = NULL;
    HANDLE source = INVALID_HANDLE_VALUE;
    nb_status status = utf8_to_wide(src, &wide_src, err);
    if (status == NB_OK)
        status = utf8_to_wide(dst, &wide_dst, err);
    if (status == NB_OK)
        status =
            win_copy_opened_file(src, dst, wide_src, wide_dst, options, source_mode, &source, err);
    win_copy_handle_cleanup(&source);
    win_copy_path_cleanup(&wide_dst);
    win_copy_path_cleanup(&wide_src);
    return status;
}

static nb_status win_copy_entry(const char *src, const char *dst,
                                const nb_file_copy_options *options, size_t depth, nb_err *err);

static nb_status win_delete_existing(const char *path, DWORD attrs,
                                     nb_err *err)
{
    wchar_t *wide = NULL;
    nb_status status = utf8_to_wide(path, &wide, err);
    if (status != NB_OK) {
        return status;
    }
    BOOL ok = DeleteFileW(wide);
    DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        ok = RemoveDirectoryW(wide);
        code = ok ? ERROR_SUCCESS : GetLastError();
    }
    wide_free(wide);
    if (!ok) {
        report_win_io_error(err, code, "remove", path);
        return NB_ERR_IO;
    }
    return NB_OK;
}

static nb_status win_copy_directory(const char *src, const char *dst,
                                    const nb_file_copy_options *options, uint32_t source_mode,
                                    size_t depth, nb_err *err)
{
    if (!options->recursive) {
        nb_err_set(err, NB_ERR_UNSUPPORTED,
                   "copying directory '%s' requires recursive mode", src);
        return NB_ERR_UNSUPPORTED;
    }
    wchar_t *wide_dst = NULL;
    nb_status status = utf8_to_wide(dst, &wide_dst, err);
    if (status != NB_OK) {
        return status;
    }
    DWORD attrs = GetFileAttributesW(wide_dst);
    bool exists = attrs != INVALID_FILE_ATTRIBUTES;
    bool created = false;
    if (exists) {
        if (!options->overwrite) {
            wide_free(wide_dst);
            nb_err_set(err, NB_ERR_EXISTS, "path '%s' already exists", dst);
            return NB_ERR_EXISTS;
        }
        bool reparse = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (reparse || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            wide_free(wide_dst);
            if (!options->overwrite) {
                nb_err_set(err, NB_ERR_EXISTS, "path '%s' already exists",
                           dst);
                return NB_ERR_EXISTS;
            }
            status = win_delete_existing(dst, attrs, err);
            if (status != NB_OK) {
                return status;
            }
            exists = false;
            status = utf8_to_wide(dst, &wide_dst, err);
            if (status != NB_OK) {
                return status;
            }
        }
    }
    if (!exists) {
        if (!CreateDirectoryW(wide_dst, NULL)) {
            DWORD code = GetLastError();
            wide_free(wide_dst);
            if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
                nb_err_set(err, NB_ERR_EXISTS, "path '%s' already exists",
                           dst);
                return NB_ERR_EXISTS;
            }
            report_win_io_error(err, code, "mkdir", dst);
            return NB_ERR_IO;
        }
        created = true;
    }
    wide_free(wide_dst);

    nb_dir *dir = nb_dir_open(src, err);
    if (!dir) {
        nb_status open_status = err ? (nb_status)err->code : NB_ERR_IO;
        if (created) {
            (void)nb_file_remove_all(dst, 0, NULL);
        }
        return open_status;
    }
    nb_buf *src_child = nb_buf_new();
    nb_buf *dst_child = nb_buf_new();
    status = NB_OK;
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
        status = win_copy_entry(nb_buf_cstr(src_child), nb_buf_cstr(dst_child), options, depth + 1u,
                                err);
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
        status = nb_file_set_mode(dst, mode, err);
    }
    if (status != NB_OK && created) {
        (void)nb_file_remove_all(dst, 0, NULL);
    }
    return status;
}

static nb_status win_copy_entry_run(const char *src, const char *dst,
                                    const nb_file_copy_options *options, size_t depth,
                                    nb_buf *target, nb_err *err)
{
    nb_file_copy_entry entry = {.depth = depth};
    nb_status status = nb_file_stat(src, false, &entry.info, err);
    if (status != NB_OK) {
        return status;
    }

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
            wchar_t *wide_dst = NULL;
            status = utf8_to_wide(dst, &wide_dst, err);
            if (status != NB_OK) {
                return status;
            }
            DWORD attrs = GetFileAttributesW(wide_dst);
            wide_free(wide_dst);
            if (attrs != INVALID_FILE_ATTRIBUTES) {
                bool reparse = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                if (!options->overwrite) {
                    nb_err_set(err, NB_ERR_EXISTS, "path '%s' already exists", dst);
                    return NB_ERR_EXISTS;
                }
                if (!reparse && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    nb_err_set(err, NB_ERR_EXISTS, "destination '%s' is a directory", dst);
                    return NB_ERR_EXISTS;
                }
                status = win_delete_existing(dst, attrs, err);
                if (status != NB_OK) {
                    return status;
                }
            }
            status = nb_file_symlink(entry.symlink_target, dst, err);
            return status;
        }

        nb_file_info target_info;
        status = nb_file_stat(src, true, &target_info, err);
        if (status != NB_OK) {
            return status;
        }
        if (target_info.is_dir) {
            nb_err_set(err, NB_ERR_UNSUPPORTED,
                       "refusing to follow directory link '%s'", src);
            return NB_ERR_UNSUPPORTED;
        }
        if (!target_info.is_regular) {
            nb_err_set(err, NB_ERR_UNSUPPORTED,
                       "cannot copy special file '%s'", src);
            return NB_ERR_UNSUPPORTED;
        }
        return win_copy_regular_file(src, dst, options, target_info.mode,
                                     err);
    }
    if (entry.info.is_regular) {
        nb_file_copy_options regular = *options;
        regular.nofollow_source = true;
        return win_copy_regular_file(src, dst, &regular, entry.info.mode, err);
    }
    if (entry.info.is_dir) {
        return win_copy_directory(src, dst, options, entry.info.mode, depth, err);
    }
    nb_err_set(err, NB_ERR_UNSUPPORTED, "cannot copy special file '%s'", src);
    return NB_ERR_UNSUPPORTED;
}

static nb_status win_copy_entry(const char *src, const char *dst,
                                const nb_file_copy_options *options, size_t depth, nb_err *err)
{
    nb_buf target;
    nb_buf_init(&target);
    nb_status status = win_copy_entry_run(src, dst, options, depth, &target, err);
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
    return win_copy_entry(src, dst, &effective, 0, err);
}

static nb_status win_remove_all_entry(const char *path, size_t remaining, nb_err *err)
{
    if (remaining == 0) {
        nb_err_set(err, NB_ERR_LOOP, "recursive removal depth exceeded");
        return NB_ERR_LOOP;
    }
    if (!path || path[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid path");
        return NB_ERR_INVAL;
    }

    size_t length = strlen(path);
    size_t root_len = nb_platform_path_root_len(path);
    while (length > root_len && nb_platform_path_is_sep(path[length - 1u])) {
        length--;
    }
    char *trimmed = NULL;
    if (length != strlen(path)) {
        trimmed = nb_try_alloc(length + 1u);
        if (!trimmed) {
            nb_err_set(err, NB_ERR_NOMEM, "out of memory removing '%s'",
                       path);
            return NB_ERR_NOMEM;
        }
        memcpy(trimmed, path, length);
        trimmed[length] = '\0';
        path = trimmed;
    }

    wchar_t *wide = NULL;
    nb_status status = utf8_to_wide(path, &wide, err);
    if (status != NB_OK) {
        if (trimmed) {
            nb_free(trimmed, length + 1u);
        }
        return status;
    }

    DWORD attrs = GetFileAttributesW(wide);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        DWORD code = GetLastError();
        wide_free(wide);
        if (trimmed) {
            nb_free(trimmed, length + 1u);
        }
        if (win_code_missing(code)) {
            return NB_OK;
        }
        report_win_io_error(err, code, "remove", path);
        return NB_ERR_IO;
    }

    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        BOOL ok = DeleteFileW(wide);
        DWORD code = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok) {
            ok = RemoveDirectoryW(wide);
            code = ok ? ERROR_SUCCESS : GetLastError();
        }
        wide_free(wide);
        if (trimmed) {
            nb_free(trimmed, length + 1u);
        }
        if (!ok && !win_code_missing(code)) {
            report_win_io_error(err, code, "remove", path);
            return NB_ERR_IO;
        }
        return NB_OK;
    }

    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        wide_free(wide);
        nb_err open_error;
        nb_err_clear(&open_error);
        nb_dir *dir = nb_dir_open(path, &open_error);
        if (!dir) {
            if (trimmed) {
                nb_free(trimmed, length + 1u);
            }
            if (open_error.code == NB_ERR_NOT_FOUND) {
                return NB_OK;
            }
            if (err) {
                *err = open_error;
            }
            return (nb_status)open_error.code;
        }
        nb_buf *child = nb_buf_new();
        nb_status status_child = NB_OK;
        if (!child) {
            nb_err_set(err, NB_ERR_NOMEM,
                       "out of memory removing directory '%s'", path);
            status_child = NB_ERR_NOMEM;
        }
        while (status_child == NB_OK) {
            const char *name = nb_dir_next(dir, NULL);
            if (!name) {
                break;
            }
            if (nb_path_join(child, path, name) != NB_OK) {
                nb_err_set(err, NB_ERR_NOMEM,
                           "out of memory removing directory '%s'", path);
                status_child = NB_ERR_NOMEM;
                break;
            }
            status_child = win_remove_all_entry(nb_buf_cstr(child), remaining - 1, err);
        }
        if (status_child == NB_OK) {
            status_child = nb_dir_status(dir);
            if (status_child != NB_OK) {
                nb_err_set(err, status_child,
                           "failed to read directory '%s'", path);
            }
        }
        nb_dir_close(dir);
        nb_buf_free(child);
        if (status_child == NB_OK) {
            status = utf8_to_wide(path, &wide, err);
            if (status == NB_OK) {
                if (!RemoveDirectoryW(wide)) {
                    DWORD code = GetLastError();
                    if (!win_code_missing(code)) {
                        report_win_io_error(err, code, "rmdir", path);
                        status = NB_ERR_IO;
                    }
                }
                wide_free(wide);
            }
            status_child = status;
        }
        if (trimmed) {
            nb_free(trimmed, length + 1u);
        }
        return status_child;
    }

    BOOL ok = DeleteFileW(wide);
    DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    wide_free(wide);
    if (trimmed) {
        nb_free(trimmed, length + 1u);
    }
    if (!ok && !win_code_missing(code)) {
        report_win_io_error(err, code, "remove", path);
        return NB_ERR_IO;
    }
    return NB_OK;
}

nb_status nb_file_remove_all(const char *path, size_t max_depth, nb_err *err)
{
    size_t remaining = max_depth && max_depth < SIZE_MAX ? max_depth + 1 : SIZE_MAX;
    return win_remove_all_entry(path, remaining, err);
}
