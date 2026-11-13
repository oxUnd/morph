#include "platform/nb_platform.h"

#include "nb/mem.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

bool nb_platform_path_is_sep(char c)
{
    return c == '/' || c == '\\';
}

bool nb_platform_path_is_absolute(const char *path)
{
    if (!path || path[0] == '\0') {
        return false;
    }
    if (nb_platform_path_is_sep(path[0])) {
        /* Rooted paths ("/x", "\\x") and UNC paths ("//server", "\\\\server"). */
        return true;
    }
    bool drive_letter =
        (path[0] >= 'A' && path[0] <= 'Z') ||
        (path[0] >= 'a' && path[0] <= 'z');
    /* "C:relative" is drive-relative, not absolute; "C:\\" and "C:/" are. */
    return drive_letter && path[1] == ':' &&
           nb_platform_path_is_sep(path[2]);
}

size_t nb_platform_path_root_len(const char *path)
{
    if (!path || path[0] == '\0') {
        return 0;
    }
    if (nb_platform_path_is_sep(path[0]) &&
        nb_platform_path_is_sep(path[1])) {
        /* UNC: \\server\share\ */
        size_t i = 2u;
        while (path[i] && !nb_platform_path_is_sep(path[i])) {
            i++;
        }
        if (i == 2u) {
            return 1u;
        }
        if (nb_platform_path_is_sep(path[i])) {
            i++;
        }
        size_t share_start = i;
        while (path[i] && !nb_platform_path_is_sep(path[i])) {
            i++;
        }
        if (i == share_start) {
            return 1u;
        }
        if (nb_platform_path_is_sep(path[i])) {
            i++;
        }
        return i;
    }
    if (((path[0] >= 'A' && path[0] <= 'Z') ||
         (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && nb_platform_path_is_sep(path[2])) {
        return 3u;
    }
    if (nb_platform_path_is_sep(path[0])) {
        return 1u;
    }
    return 0u;
}

/* Length of a drive-relative prefix such as "C:", or 0 when the path is not
 * drive-relative. "C:" and "C:foo" are relative to the per-drive current
 * directory; "C:\\foo" is handled by nb_platform_path_root_len(). */
size_t nb_platform_path_drive_len(const char *path)
{
    if (!path || path[0] == '\0') {
        return 0u;
    }
    bool drive_letter =
        (path[0] >= 'A' && path[0] <= 'Z') ||
        (path[0] >= 'a' && path[0] <= 'z');
    if (drive_letter && path[1] == ':' &&
        !nb_platform_path_is_sep(path[2])) {
        return 2u;
    }
    return 0u;
}

static nb_status getcwd_error(nb_err *err, DWORD code, const char *what)
{
    nb_err_set_os(err, NB_ERR_IO, (int)code,
                  "%s failed (win32 error %lu)", what,
                  (unsigned long)code);
    return NB_ERR_IO;
}

nb_status nb_platform_getcwd(nb_buf *out, nb_err *err)
{
    if (!out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid output buffer");
        return NB_ERR_INVAL;
    }
    nb_buf_clear(out);

    size_t cap = 256u;
    wchar_t *wide = nb_try_alloc(cap * sizeof(wchar_t));
    if (!wide) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory resolving path");
        return NB_ERR_NOMEM;
    }

    for (;;) {
        DWORD needed = GetCurrentDirectoryW((DWORD)cap, wide);
        if (needed == 0) {
            DWORD code = GetLastError();
            nb_free(wide, cap * sizeof(wchar_t));
            return getcwd_error(err, code, "GetCurrentDirectoryW");
        }
        if ((size_t)needed < cap) {
            break;
        }
        size_t new_cap = (size_t)needed;
        if (new_cap <= cap) {
            new_cap = cap + 1u;
        }
        wchar_t *grown = nb_try_alloc(new_cap * sizeof(wchar_t));
        if (!grown) {
            nb_free(wide, cap * sizeof(wchar_t));
            nb_err_set(err, NB_ERR_NOMEM, "out of memory resolving path");
            return NB_ERR_NOMEM;
        }
        nb_free(wide, cap * sizeof(wchar_t));
        wide = grown;
        cap = new_cap;
    }

    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (bytes <= 0) {
        DWORD code = GetLastError();
        nb_free(wide, cap * sizeof(wchar_t));
        return getcwd_error(err, code, "WideCharToMultiByte");
    }
    char *utf8 = nb_try_alloc((size_t)bytes);
    if (!utf8) {
        nb_free(wide, cap * sizeof(wchar_t));
        nb_err_set(err, NB_ERR_NOMEM, "out of memory resolving path");
        return NB_ERR_NOMEM;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, bytes, NULL, NULL) !=
        bytes) {
        DWORD code = GetLastError();
        nb_free(utf8, (size_t)bytes);
        nb_free(wide, cap * sizeof(wchar_t));
        return getcwd_error(err, code, "WideCharToMultiByte");
    }

    size_t len = (size_t)bytes - 1u;
    for (size_t i = 0; i < len; i++) {
        if (utf8[i] == '\\') {
            utf8[i] = '/';
        }
    }
    bool ok = nb_buf_append(out, utf8, len);
    nb_free(utf8, (size_t)bytes);
    nb_free(wide, cap * sizeof(wchar_t));
    if (!ok) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory resolving path");
        return NB_ERR_NOMEM;
    }
    return NB_OK;
}

static void win_free_wide(wchar_t *wide, size_t chars)
{
    if (wide) {
        nb_free(wide, chars * sizeof(wchar_t));
    }
}

static nb_status win_utf8_to_wide(const char *text, wchar_t **out,
                                  size_t *chars_out, nb_err *err)
{
    *out = NULL;
    if (chars_out) {
        *chars_out = 0;
    }
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                                     NULL, 0);
    if (needed <= 0) {
        DWORD code = GetLastError();
        nb_status status =
            code == ERROR_NO_UNICODE_TRANSLATION ? NB_ERR_INVAL : NB_ERR_IO;
        nb_err_set_os(err, status, (int)code, "invalid UTF-8 path");
        return status;
    }
    wchar_t *wide = nb_try_alloc((size_t)needed * sizeof(wchar_t));
    if (!wide) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory converting path");
        return NB_ERR_NOMEM;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide,
                            needed) != needed) {
        DWORD code = GetLastError();
        win_free_wide(wide, (size_t)needed);
        nb_err_set_os(err, NB_ERR_IO, (int)code, "cannot convert path");
        return NB_ERR_IO;
    }
    *out = wide;
    if (chars_out) {
        *chars_out = (size_t)needed;
    }
    return NB_OK;
}

static nb_status win_wide_utf8_to_buf(nb_buf *out, const wchar_t *wide,
                                      bool clear, nb_err *err)
{
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) {
        DWORD code = GetLastError();
        nb_err_set_os(err, NB_ERR_IO, (int)code, "cannot convert path");
        return NB_ERR_IO;
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
        nb_err_set_os(err, NB_ERR_IO, (int)code, "cannot convert path");
        return NB_ERR_IO;
    }

    size_t len = (size_t)needed - 1u;
    if (clear) {
        nb_buf_clear(out);
    }
    bool ok = nb_buf_reserve(out, len);
    for (size_t i = 0; ok && i < len; i++) {
        char c = utf8[i] == '\\' ? '/' : utf8[i];
        ok = nb_buf_append_char(out, c);
    }
    nb_free(utf8, (size_t)needed);
    if (!ok) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory converting path");
        return NB_ERR_NOMEM;
    }
    return NB_OK;
}

static nb_status win_append_wide_utf8(nb_buf *out, const wchar_t *wide,
                                      nb_err *err)
{
    return win_wide_utf8_to_buf(out, wide, true, err);
}

bool nb_platform_path_prefix_equal(const char *a, const char *b, size_t len)
{
    if (!a || !b) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

nb_status nb_platform_path_canonical(nb_buf *out, const char *path,
                                     nb_err *err)
{
    if (!out || !path || path[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or output buffer");
        return NB_ERR_INVAL;
    }
    nb_buf_clear(out);

    wchar_t *wide = NULL;
    size_t wide_chars = 0;
    nb_status status = win_utf8_to_wide(path, &wide, &wide_chars, err);
    if (status != NB_OK) {
        return status;
    }
    HANDLE handle = CreateFileW(
        wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        win_free_wide(wide, wide_chars);
        nb_status mapped =
            (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
                ? NB_ERR_NOT_FOUND
                : NB_ERR_IO;
        nb_err_set_os(err, mapped, (int)code, "open '%s' failed (win32 %lu)",
                      path, (unsigned long)code);
        return mapped;
    }
    win_free_wide(wide, wide_chars);

    size_t cap = 256u;
    wchar_t *resolved = NULL;
    for (;;) {
        resolved = nb_try_alloc(cap * sizeof(wchar_t));
        if (!resolved) {
            (void)CloseHandle(handle);
            nb_err_set(err, NB_ERR_NOMEM, "out of memory resolving '%s'",
                       path);
            return NB_ERR_NOMEM;
        }
        DWORD needed = GetFinalPathNameByHandleW(
            handle, resolved, (DWORD)cap,
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (needed == 0) {
            DWORD code = GetLastError();
            win_free_wide(resolved, cap);
            (void)CloseHandle(handle);
            nb_err_set_os(err, NB_ERR_IO, (int)code,
                          "GetFinalPathNameByHandleW '%s' failed (win32 %lu)",
                          path, (unsigned long)code);
            return NB_ERR_IO;
        }
        if ((size_t)needed < cap) {
            break;
        }
        win_free_wide(resolved, cap);
        cap = (size_t)needed + 1u;
    }
    (void)CloseHandle(handle);

    bool unc = wcsncmp(resolved, L"\\\\?\\UNC\\", 8) == 0;
    bool extended = wcsncmp(resolved, L"\\\\?\\", 4) == 0;
    const wchar_t *source = resolved;
    nb_buf_clear(out);
    if (unc) {
        if (!nb_buf_append_cstr(out, "//")) {
            win_free_wide(resolved, cap);
            nb_err_set(err, NB_ERR_NOMEM, "out of memory resolving '%s'",
                       path);
            return NB_ERR_NOMEM;
        }
        source = resolved + 8;
    } else if (extended) {
        source = resolved + 4;
    }

    nb_status convert_status = win_wide_utf8_to_buf(out, source, false, err);
    win_free_wide(resolved, cap);
    return convert_status;
}

nb_status nb_platform_path_executable(nb_buf *out, nb_err *err)
{
    if (!out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid output buffer");
        return NB_ERR_INVAL;
    }
    nb_buf_clear(out);

    size_t cap = 256u;
    for (;;) {
        wchar_t *wide = nb_try_alloc(cap * sizeof(wchar_t));
        if (!wide) {
            nb_err_set(err, NB_ERR_NOMEM,
                       "out of memory resolving executable path");
            return NB_ERR_NOMEM;
        }
        DWORD len = GetModuleFileNameW(NULL, wide, (DWORD)cap);
        if (len == 0) {
            DWORD code = GetLastError();
            win_free_wide(wide, cap);
            nb_err_set_os(err, NB_ERR_IO, (int)code,
                          "GetModuleFileNameW failed (win32 %lu)",
                          (unsigned long)code);
            return NB_ERR_IO;
        }
        if ((size_t)len < cap) {
            nb_status status = win_append_wide_utf8(out, wide, err);
            win_free_wide(wide, cap);
            return status;
        }
        win_free_wide(wide, cap);
        cap *= 2u;
    }
}

nb_status nb_platform_path_temp_dir(nb_buf *out, nb_err *err)
{
    if (!out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid output buffer");
        return NB_ERR_INVAL;
    }
    nb_buf_clear(out);

    size_t cap = (size_t)MAX_PATH + 1u;
    wchar_t *wide = NULL;
    for (;;) {
        wide = nb_try_alloc(cap * sizeof(wchar_t));
        if (!wide) {
            nb_err_set(err, NB_ERR_NOMEM,
                       "out of memory resolving temp path");
            return NB_ERR_NOMEM;
        }
        DWORD needed = GetTempPathW((DWORD)cap, wide);
        if (needed == 0) {
            DWORD code = GetLastError();
            win_free_wide(wide, cap);
            nb_err_set_os(err, NB_ERR_IO, (int)code,
                          "GetTempPathW failed (win32 %lu)",
                          (unsigned long)code);
            return NB_ERR_IO;
        }
        if ((size_t)needed < cap) {
            break;
        }
        win_free_wide(wide, cap);
        cap = (size_t)needed + 1u;
    }

    nb_status status = win_append_wide_utf8(out, wide, err);
    win_free_wide(wide, cap);
    if (status != NB_OK) {
        return status;
    }
    size_t root_len = nb_platform_path_root_len(nb_buf_cstr(out));
    while (nb_buf_len(out) > root_len &&
           nb_platform_path_is_sep(out->ptr[nb_buf_len(out) - 1u])) {
        out->len--;
        out->ptr[out->len] = '\0';
    }
    return NB_OK;
}
