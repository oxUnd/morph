#include "nb/file.h"

#include "nb/buf.h"
#include "nb/mem.h"
#include "platform/nb_platform.h"

#include <string.h>
#include <stdlib.h>
#include "nb/cleanup.h"

static nb_status path_join_into(nb_buf *out, const char *a, const char *b)
{
    if (!out || !b) {
        return NB_ERR_INVAL;
    }
    nb_buf_clear(out);
    if (!a || a[0] == '\0' || nb_platform_path_is_absolute(b)) {
        return nb_buf_append_cstr(out, b) ? NB_OK : NB_ERR_NOMEM;
    }
    size_t a_len = strlen(a);
    bool trailing_sep =
        a_len > 0 && nb_platform_path_is_sep(a[a_len - 1u]);
    bool drive_prefix = nb_platform_path_root_len(a) == 0 &&
                        nb_platform_path_drive_len(a) == a_len;
    if (!nb_buf_append_cstr(out, a) ||
        (!trailing_sep && !drive_prefix &&
         !nb_buf_append_char(out, '/')) ||
        !nb_buf_append_cstr(out, b)) {
        return NB_ERR_NOMEM;
    }
    return NB_OK;
}

static bool append_clean_segment(nb_buf *out, const char *seg, size_t len,
                                 bool needs_sep)
{
    if (needs_sep && nb_buf_len(out) > 0 && out->ptr[out->len - 1u] != '/' &&
        !nb_buf_append_char(out, '/')) {
        return false;
    }
    return nb_buf_append(out, seg, len);
}

static bool last_segment_is_parent(const nb_buf *out, size_t prefix_len)
{
    size_t len = nb_buf_len(out);
    if (len < prefix_len + 2u || out->ptr[len - 1u] != '.' ||
        out->ptr[len - 2u] != '.') {
        return false;
    }
    return len == prefix_len + 2u || out->ptr[len - 3u] == '/';
}

static void pop_clean_segment(nb_buf *out, size_t root_len)
{
    size_t i = nb_buf_len(out);
    while (i > root_len && out->ptr[i - 1u] != '/') {
        i--;
    }
    size_t len;
    if (i <= root_len) {
        len = root_len;
    } else {
        len = i - 1u;
        if (len < root_len) {
            len = root_len;
        }
    }
    out->len = len;
    out->off = 0;
    if (out->ptr) {
        out->ptr[len] = '\0';
    }
}

static nb_status path_clean_into(nb_buf *out, const char *path)
{
    if (!out || !path) {
        return NB_ERR_INVAL;
    }
    nb_buf_clear(out);
    if (path[0] == '\0') {
        return NB_ERR_INVAL;
    }

    size_t root_len = nb_platform_path_root_len(path);
    size_t drive_len =
        root_len == 0 ? nb_platform_path_drive_len(path) : 0;
    size_t prefix_len = root_len > 0 ? root_len : drive_len;
    bool absolute = root_len > 0;
    bool drive_relative = drive_len > 0;
    for (size_t i = 0; i < prefix_len; i++) {
        char c = path[i];
        if (nb_platform_path_is_sep(c)) {
            c = '/';
        }
        if (!nb_buf_append_char(out, c)) {
            return NB_ERR_NOMEM;
        }
    }

    size_t segments = 0;
    const char *p = path + prefix_len;
    while (*p) {
        while (nb_platform_path_is_sep(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *start = p;
        while (*p && !nb_platform_path_is_sep(*p)) {
            p++;
        }
        size_t len = (size_t)(p - start);

        if (len == 1u && start[0] == '.') {
            continue;
        }
        if (len == 2u && start[0] == '.' && start[1] == '.') {
            if (segments == 0) {
                if (absolute) {
                    continue;
                }
            } else if (!last_segment_is_parent(out, prefix_len)) {
                pop_clean_segment(out, prefix_len);
                segments--;
                continue;
            }
            if (!append_clean_segment(out, "..", 2u,
                                      !(drive_relative && segments == 0))) {
                return NB_ERR_NOMEM;
            }
            segments++;
            continue;
        }
        if (!append_clean_segment(out, start, len,
                                  !(drive_relative && segments == 0))) {
            return NB_ERR_NOMEM;
        }
        segments++;
    }

    if (nb_buf_len(out) == 0 && !nb_buf_append_char(out, '.')) {
        return NB_ERR_NOMEM;
    }
    return NB_OK;
}

static nb_status path_absolute_into(nb_buf *out, const char *path, nb_err *err)
{
    if (!out || !path) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or output buffer");
        return NB_ERR_INVAL;
    }
    if (nb_platform_path_is_absolute(path)) {
        nb_buf_clear(out);
        if (!nb_buf_append_cstr(out, path)) {
            nb_err_set(err, NB_ERR_NOMEM, "out of memory building path");
            return NB_ERR_NOMEM;
        }
        return NB_OK;
    }

    nb_buf *cwd = nb_buf_new();
    if (!cwd) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory resolving path");
        return NB_ERR_NOMEM;
    }
    nb_status status = nb_platform_getcwd(cwd, err);
    if (status == NB_OK) {
        status = path_join_into(out, nb_buf_cstr(cwd), path);
        if (status != NB_OK) {
            nb_err_set(err, status, "out of memory building path");
        }
    }
    nb_buf_free(cwd);
    return status;
}

/* Length of the absolute root or Windows drive-relative prefix ("C:"). */
static size_t path_prefix_len(const char *path)
{
    size_t root_len = nb_platform_path_root_len(path);
    return root_len > 0 ? root_len : nb_platform_path_drive_len(path);
}

static size_t path_trimmed_len(const char *path, size_t prefix_len)
{
    size_t len = strlen(path);
    while (len > prefix_len && nb_platform_path_is_sep(path[len - 1u])) {
        len--;
    }
    return len;
}

static size_t path_basename_offset(const char *path, size_t prefix_len,
                                   size_t end)
{
    size_t start = prefix_len;
    for (size_t i = prefix_len; i < end; i++) {
        if (nb_platform_path_is_sep(path[i])) {
            start = i + 1u;
        }
    }
    return start;
}

static nb_status path_split_into(const char *path, nb_str *dir_out,
                                 nb_str *name_out)
{
    if (!path || path[0] == '\0') {
        return NB_ERR_INVAL;
    }

    size_t prefix_len = path_prefix_len(path);
    size_t end = path_trimmed_len(path, prefix_len);
    size_t base = path_basename_offset(path, prefix_len, end);
    size_t dir_len;
    if (base > prefix_len) {
        dir_len = base - 1u;
        while (dir_len > prefix_len &&
               nb_platform_path_is_sep(path[dir_len - 1u])) {
            dir_len--;
        }
    } else {
        dir_len = prefix_len;
    }

    if (dir_out) {
        *dir_out = nb_str_from_parts(path, dir_len);
    }
    if (name_out) {
        *name_out = nb_str_from_parts(path + base, end - base);
    }
    return NB_OK;
}

nb_status nb_path_join(nb_buf *out, const char *a, const char *b)
{
    if (!out || !b) {
        return NB_ERR_INVAL;
    }
    nb_buf temporary;
    nb_buf_init(&temporary);
    nb_status status = path_join_into(&temporary, a, b);
    if (status == NB_OK) {
        nb_buf_move(out, &temporary);
    }
    nb_buf_destroy(&temporary);
    return status;
}

nb_status nb_path_clean(nb_buf *out, const char *path)
{
    if (!out || !path || !*path) {
        return NB_ERR_INVAL;
    }
    nb_buf temporary;
    nb_buf_init(&temporary);
    nb_status status = path_clean_into(&temporary, path);
    if (status == NB_OK) {
        nb_buf_move(out, &temporary);
    }
    nb_buf_destroy(&temporary);
    return status;
}

nb_status nb_path_absolute(nb_buf *out, const char *path, nb_err *err)
{
    if (!out || !path) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or output buffer");
        return NB_ERR_INVAL;
    }
    nb_buf temporary;
    nb_buf_init(&temporary);
    nb_status status = path_absolute_into(&temporary, path, err);
    if (status == NB_OK) {
        nb_buf_move(out, &temporary);
    }
    nb_buf_destroy(&temporary);
    return status;
}

static nb_status append_missing_tail(nb_buf *out, const char *tail, size_t length, nb_err *err)
{
    size_t index = 0;
    while (index < length) {
        while (index < length && nb_platform_path_is_sep(tail[index])) {
            index++;
        }
        size_t start = index;
        while (index < length && !nb_platform_path_is_sep(tail[index])) {
            index++;
        }
        size_t count = index - start;
        if (count == 0 || (count == 1 && tail[start] == '.')) {
            continue;
        }
        if (count == 2 && tail[start] == '.' && tail[start + 1u] == '.') {
            nb_err_set(err, NB_ERR_INVAL, "cannot resolve '..' in a path that does not exist");
            return NB_ERR_INVAL;
        }
        if (nb_buf_len(out) > 0 && out->ptr[out->len - 1u] != '/' &&
            !nb_buf_append_char(out, '/')) {
            return NB_ERR_NOMEM;
        }
        if (!nb_buf_append(out, tail + start, count)) {
            return NB_ERR_NOMEM;
        }
    }
    return NB_OK;
}

nb_status nb_path_canonical_missing(nb_buf *out, const char *path, nb_err *err)
{
    if (!out || !path || path[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or output buffer");
        return NB_ERR_INVAL;
    }

    nb_buf absolute;
    nb_buf_init(&absolute);
    nb_status status = nb_path_absolute(&absolute, path, err);
    if (status != NB_OK) {
        nb_buf_destroy(&absolute);
        return status;
    }

    const char *text = nb_buf_cstr(&absolute);
    size_t length = nb_buf_len(&absolute);
    size_t root = nb_platform_path_root_len(text);
    if (!root)
        root = 1;
    size_t cut = length;
    nb_buf probe;
    nb_buf_init(&probe);
    nb_file_info info;
    /* Windows resolves "missing/.." lexically in filesystem APIs. Verify
     * every parent step's prefix exists before allowing that normalization,
     * so an uncreated component cannot disappear before the tail check. */
    for (size_t start = root; start < length;) {
        while (start < length && nb_platform_path_is_sep(text[start]))
            start++;
        size_t end = start;
        while (end < length && !nb_platform_path_is_sep(text[end]))
            end++;
        if (end - start == 2 && text[start] == '.' && text[start + 1] == '.') {
            nb_buf_clear(&probe);
            if (!nb_buf_append(&probe, text, start)) {
                status = NB_ERR_NOMEM;
                break;
            }
            status = nb_file_stat(nb_buf_cstr(&probe), true, &info, err);
            if (status == NB_ERR_NOT_FOUND) {
                status = NB_ERR_INVAL;
                nb_err_set(err, status, "cannot resolve '..' after a missing component");
            } else if (status == NB_OK && !info.is_dir) {
                status = NB_ERR_NOT_DIR;
                nb_err_set(err, status, "parent step traverses a nondirectory");
            }
            if (status != NB_OK)
                break;
        }
        start = end;
    }
    while (status == NB_OK) {
        nb_buf_clear(&probe);
        if (!nb_buf_append(&probe, text, cut)) {
            status = NB_ERR_NOMEM;
            break;
        }
        status = nb_file_stat(nb_buf_cstr(&probe), false, &info, err);
        if (status == NB_OK) {
            break;
        }
        if (status != NB_ERR_NOT_FOUND || cut <= root) {
            break;
        }
        while (cut > root && !nb_platform_path_is_sep(text[cut - 1u])) {
            cut--;
        }
        if (cut > root) {
            cut--;
        }
        status = NB_OK;
    }

    nb_buf canonical;
    nb_buf_init(&canonical);
    if (status == NB_OK) {
        status = nb_path_canonical(&canonical, nb_buf_cstr(&probe), err);
    }
    if (status == NB_OK) {
        status = append_missing_tail(&canonical, text + cut, length - cut, err);
    }
    if (status == NB_OK) {
        nb_buf_move(out, &canonical);
    }
    nb_buf_destroy(&canonical);
    nb_buf_destroy(&probe);
    nb_buf_destroy(&absolute);
    return status;
}

bool nb_path_is_absolute(const char *path)
{
    return path != NULL && path[0] != '\0' &&
           nb_platform_path_is_absolute(path);
}

nb_status nb_path_split(const char *path, nb_str *dir_out, nb_str *name_out)
{
    return path_split_into(path, dir_out, name_out);
}

nb_status nb_path_dirname(nb_buf *out, const char *path)
{
    if (!out) {
        return NB_ERR_INVAL;
    }
    nb_str dir;
    nb_status status = path_split_into(path, &dir, NULL);
    if (status != NB_OK) {
        return status;
    }

    nb_buf temporary;
    nb_buf_init(&temporary);
    bool ok = dir.len > 0 ? nb_buf_append(&temporary, dir.ptr, dir.len)
                          : nb_buf_append_char(&temporary, '.');
    if (!ok) {
        nb_buf_destroy(&temporary);
        return NB_ERR_NOMEM;
    }
    nb_buf_move(out, &temporary);
    nb_buf_destroy(&temporary);
    return NB_OK;
}

nb_status nb_path_set_extension(nb_buf *out, const char *path,
                                const char *extension)
{
    if (!out) {
        return NB_ERR_INVAL;
    }
    nb_str name;
    nb_status status = path_split_into(path, NULL, &name);
    if (status != NB_OK) {
        return status;
    }

    size_t path_len = strlen(path);
    if (name.len == 0 || nb_platform_path_is_sep(path[path_len - 1u])) {
        return NB_ERR_INVAL;
    }
    if ((name.len == 1u && name.ptr[0] == '.') ||
        (name.len == 2u && name.ptr[0] == '.' && name.ptr[1] == '.')) {
        return NB_ERR_INVAL;
    }

    const char *ext = extension ? extension : "";
    while (*ext == '.') {
        ext++;
    }
    for (const char *p = ext; *p; p++) {
        if (nb_platform_path_is_sep(*p)) {
            return NB_ERR_INVAL;
        }
    }

    size_t base = (size_t)(name.ptr - path);
    size_t dot = name.len;
    for (size_t i = 0; i < name.len; i++) {
        if (name.ptr[i] == '.') {
            dot = i;
        }
    }
    if (dot == 0) {
        /* A leading dot marks a hidden file, not an extension. */
        dot = name.len;
    }
    size_t keep = base + dot;

    nb_buf temporary;
    nb_buf_init(&temporary);
    bool ok = nb_buf_append(&temporary, path, keep);
    if (ok && ext[0] != '\0') {
        ok = nb_buf_append_char(&temporary, '.') &&
             nb_buf_append_cstr(&temporary, ext);
    }
    if (!ok) {
        nb_buf_destroy(&temporary);
        return NB_ERR_NOMEM;
    }
    nb_buf_move(out, &temporary);
    nb_buf_destroy(&temporary);
    return NB_OK;
}

const char *nb_path_basename(const char *path)
{
    if (!path) {
        return "";
    }
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (nb_platform_path_is_sep(*p)) {
            base = p + 1;
        }
    }
    return base;
}

const char *nb_path_extension(const char *path)
{
    const char *base = nb_path_basename(path);
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) {
        return NULL;
    }
    return dot + 1;
}

nb_status nb_path_canonical(nb_buf *out, const char *path, nb_err *err)
{
    if (!out || !path || path[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or output buffer");
        return NB_ERR_INVAL;
    }

    nb_buf temporary;
    nb_buf_init(&temporary);
    nb_status status = nb_platform_path_canonical(&temporary, path, err);
    if (status == NB_OK) {
        nb_buf_move(out, &temporary);
    }
    nb_buf_destroy(&temporary);
    return status;
}

static bool canonical_path_within(const nb_buf *path, const nb_buf *root)
{
    const char *root_text = nb_buf_cstr(root);
    const char *path_text = nb_buf_cstr(path);
    size_t root_len = nb_buf_len(root);
    size_t path_len = nb_buf_len(path);
    return root_len > 0 && path_len >= root_len &&
           nb_platform_path_prefix_equal(path_text, root_text, root_len) &&
           (path_len == root_len || nb_platform_path_is_sep(root_text[root_len - 1]) ||
            nb_platform_path_is_sep(path_text[root_len]));
}

bool nb_path_is_within(const char *path, const char *root, bool allow_missing)
{
    nb_buf canonical_path = {0};
    nb_buf canonical_root = {0};
    nb_status (*canonicalize)(nb_buf *, const char *, nb_err *) =
        allow_missing ? nb_path_canonical_missing : nb_path_canonical;
    bool within = canonicalize(&canonical_path, path, NULL) == NB_OK &&
                  canonicalize(&canonical_root, root, NULL) == NB_OK &&
                  canonical_path_within(&canonical_path, &canonical_root);
    nb_buf_destroy(&canonical_root);
    nb_buf_destroy(&canonical_path);
    return within;
}

nb_status nb_path_relative_under(nb_buf *out, const char *path, const char *root,
                                 bool allow_missing, nb_err *err)
{
    if (!out) {
        nb_err_set(err, NB_ERR_INVAL, "output buffer is required");
        return NB_ERR_INVAL;
    }
    nb_buf canonical_path = {0};
    nb_buf canonical_root = {0};
    nb_buf result = {0};
    nb_status (*canonicalize)(nb_buf *, const char *, nb_err *) =
        allow_missing ? nb_path_canonical_missing : nb_path_canonical;
    nb_status status = canonicalize(&canonical_path, path, err);
    if (status == NB_OK)
        status = canonicalize(&canonical_root, root, err);
    if (status == NB_OK && !canonical_path_within(&canonical_path, &canonical_root)) {
        status = NB_ERR_PERMISSION;
        nb_err_set(err, status, "path is outside the root");
    }
    if (status == NB_OK) {
        size_t offset = nb_buf_len(&canonical_root);
        const char *text = nb_buf_cstr(&canonical_path);
        if (offset < nb_buf_len(&canonical_path) && nb_platform_path_is_sep(text[offset]))
            offset++;
        if (!nb_buf_append(&result, text + offset, nb_buf_len(&canonical_path) - offset))
            status = NB_ERR_NOMEM;
    }
    if (status == NB_OK)
        nb_buf_move(out, &result);
    nb_buf_destroy(&result);
    nb_buf_destroy(&canonical_root);
    nb_buf_destroy(&canonical_path);
    return status;
}

nb_status nb_path_executable(nb_buf *out, nb_err *err)
{
    if (!out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid output buffer");
        return NB_ERR_INVAL;
    }

    nb_buf temporary;
    nb_buf_init(&temporary);
    nb_status status = nb_platform_path_executable(&temporary, err);
    if (status == NB_OK) {
        nb_buf_move(out, &temporary);
    }
    nb_buf_destroy(&temporary);
    return status;
}

nb_status nb_path_temp_dir(nb_buf *out, nb_err *err)
{
    if (!out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid output buffer");
        return NB_ERR_INVAL;
    }

    nb_buf temporary;
    nb_buf_init(&temporary);
    nb_status status = nb_platform_path_temp_dir(&temporary, err);
    if (status == NB_OK) {
        nb_buf_move(out, &temporary);
    }
    nb_buf_destroy(&temporary);
    return status;
}

nb_status nb_path_concat(nb_buf *out, const char *a, const char *b)
{
    if (!out || !a || !b) {
        return NB_ERR_INVAL;
    }
    nb_buf result = {0};
    size_t a_len = strlen(a);
    size_t b_len = strlen(b);
    bool a_sep = a_len && nb_platform_path_is_sep(a[a_len - 1]);
    bool b_sep = b_len && nb_platform_path_is_sep(b[0]);
    if (!nb_buf_append_cstr(&result, a) ||
        (a_len && b_len && !a_sep && !b_sep && !nb_buf_append_char(&result, '/')) ||
        !nb_buf_append_cstr(&result, b + (a_sep && b_sep ? 1 : 0))) {
        nb_buf_destroy(&result);
        return NB_ERR_NOMEM;
    }
    nb_buf_move(out, &result);
    nb_buf_destroy(&result);
    return NB_OK;
}

nb_status nb_path_expand(nb_buf *out, const char *path, nb_err *err)
{
    if (!out || !path) {
        nb_err_set(err, NB_ERR_INVAL, "path and output buffer are required");
        return NB_ERR_INVAL;
    }
    nb_buf result = {0};
    if (path[0] == '~') {
        const char *home_dir = getenv("HOME");
        if (!home_dir || !home_dir[0]) {
            home_dir = getenv("USERPROFILE");
        }
        if (!home_dir || !home_dir[0]) {
            nb_err_set(err, NB_ERR_NOT_FOUND, "home directory is unavailable");
            nb_buf_destroy(&result);
            return NB_ERR_NOT_FOUND;
        }
        if (!nb_buf_append_cstr(&result, home_dir)) {
            nb_buf_destroy(&result);
            return NB_ERR_NOMEM;
        }
        path++;
    }
    if (!nb_buf_append_cstr(&result, path)) {
        nb_buf_destroy(&result);
        return NB_ERR_NOMEM;
    }
    nb_buf_move(out, &result);
    nb_buf_destroy(&result);
    return NB_OK;
}
