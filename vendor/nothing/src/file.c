#include "nb/file.h"
#include "nb/cleanup.h"
#include <stdlib.h>
#include <errno.h>

#include "platform/nb_platform.h"

nb_status nb_file_read_all(const char *path, nb_buf *out, size_t max_bytes, nb_err *err)
{
    if (!path || !out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid path or output buffer");
        return NB_ERR_INVAL;
    }
    nb_buf temporary;
    nb_buf_init(&temporary);
    nb_status status = nb_platform_file_read_all(path, &temporary, max_bytes, err);
    if (status == NB_OK) {
        nb_buf_move(out, &temporary);
    }
    nb_buf_destroy(&temporary);
    return status;
}

void nb_dir_listing_destroy(nb_dir_listing *listing)
{
    if (!listing) {
        return;
    }
    for (size_t i = 0; i < listing->names.len; i++) {
        nb_buf_destroy(nb_vec_at(&listing->names, i));
    }
    nb_vec_destroy(&listing->names);
    listing->initialized = false;
}

const char *nb_dir_listing_name(const nb_dir_listing *listing, size_t index)
{
    if (!listing) {
        return NULL;
    }
    const nb_buf *name = nb_vec_at(&listing->names, index);
    return name ? nb_buf_cstr(name) : NULL;
}

static int compare_directory_names(const void *left, const void *right)
{
    return nb_str_cmp(nb_buf_str(left), nb_buf_str(right));
}

nb_status nb_dir_scan(const char *path, unsigned flags, nb_dir_listing *out, nb_err *err)
{
    const unsigned valid = NB_DIR_SCAN_FILES | NB_DIR_SCAN_DIRS | NB_DIR_SCAN_HIDDEN |
                           NB_DIR_SCAN_FOLLOW_LINKS | NB_DIR_SCAN_SORT | NB_DIR_SCAN_ALL;
    if (!path || !out || (flags & ~valid) ||
        !(flags & (NB_DIR_SCAN_FILES | NB_DIR_SCAN_DIRS | NB_DIR_SCAN_ALL))) {
        nb_err_set(err, NB_ERR_INVAL, "invalid directory scan arguments");
        return NB_ERR_INVAL;
    }
    if (out->initialized || out->names.data) {
        nb_err_set(err, NB_ERR_STATE, "directory listing is occupied");
        return NB_ERR_STATE;
    }
    nb_err open_error = {0};
    nb_dir *directory = nb_dir_open(path, &open_error);
    if (!directory) {
        if (err) {
            *err = open_error;
        }
        return open_error.code;
    }
    nb_dir_listing result = {0};
    nb_buf full = {0};
    nb_vec_init(&result.names, sizeof(full));
    const char *name;
    nb_status status = NB_OK;
    while ((name = nb_dir_next(directory, NULL)) != NULL) {
        if (name[0] == '.' && !(flags & NB_DIR_SCAN_HIDDEN)) {
            continue;
        }
        status = nb_path_concat(&full, path, name);
        if (status != NB_OK) {
            break;
        }
        nb_file_info info;
        if (nb_file_stat(nb_buf_cstr(&full), (flags & NB_DIR_SCAN_FOLLOW_LINKS) != 0, &info,
                         NULL) != NB_OK) {
            continue;
        }
        if (!(flags & NB_DIR_SCAN_ALL) && !((info.is_regular && (flags & NB_DIR_SCAN_FILES)) ||
                                            (info.is_dir && (flags & NB_DIR_SCAN_DIRS)))) {
            continue;
        }
        nb_buf *slot = nb_vec_push(&result.names);
        if (!slot) {
            status = NB_ERR_NOMEM;
            break;
        }
        nb_buf_init(slot);
        if (!nb_buf_append_cstr(slot, name)) {
            status = NB_ERR_NOMEM;
            break;
        }
    }
    if (status == NB_OK) {
        status = nb_dir_status(directory);
    }
    nb_dir_close(directory);
    if (status != NB_OK) {
        nb_err_set(err, status, "directory scan failed");
        nb_buf_destroy(&full);
        nb_dir_listing_destroy(&result);
        return status;
    }
    if ((flags & NB_DIR_SCAN_SORT) && result.names.len > 1) {
        qsort(result.names.data, result.names.len, result.names.elem_size, compare_directory_names);
    }
    result.initialized = true;
    *out = result;
    nb_buf_destroy(&full);
    return NB_OK;
}

nb_status nb_pipe_open(int descriptors[2], nb_err *err)
{
    if (!descriptors)
        return NB_ERR_INVAL;
    descriptors[0] = -1;
    descriptors[1] = -1;
    return nb_platform_pipe_open(descriptors, err);
}
nb_status nb_fd_write_until(int fd, const void *data, size_t len, int64_t deadline_ns,
                            int (*cancelled)(void *), void *ctx, nb_err *err)
{
    if (fd < 0 || (!data && len))
        return NB_ERR_INVAL;
    return nb_platform_fd_write_until(fd, data, len, deadline_ns, cancelled, ctx, err);
}
