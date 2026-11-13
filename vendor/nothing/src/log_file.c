#include "nb/log.h"

#include "nb/buf.h"
#include "nb/mem.h"
#include "nb/thread.h"

#include <stdio.h>
#include <string.h>

struct nb_log_file {
    nb_mutex *mutex;
    FILE *stream;
    char *path;
    size_t max_bytes;
    unsigned max_files;
    size_t size;
};

static char *duplicate_cstr(const char *text)
{
    size_t len = strlen(text);
    char *copy = nb_try_alloc(len + 1u);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, text, len + 1u);
    return copy;
}

static bool open_stream(nb_log_file *file)
{
    file->stream = fopen(file->path, "a");
    if (!file->stream) {
        return false;
    }
    if (fseek(file->stream, 0, SEEK_END) != 0) {
        (void)fclose(file->stream);
        file->stream = NULL;
        return false;
    }
    long position = ftell(file->stream);
    file->size = position > 0 ? (size_t)position : 0u;
    return true;
}

static bool path_with_suffix(nb_buf *out, const char *path, unsigned index)
{
    nb_buf_clear(out);
    return nb_buf_append_cstr(out, path) && nb_buf_printf(out, ".%u", index);
}

static void rotate(nb_log_file *file)
{
    if (file->stream) {
        (void)fclose(file->stream);
        file->stream = NULL;
    }

    nb_buf source;
    nb_buf target;
    nb_buf_init(&source);
    nb_buf_init(&target);
    for (unsigned i = file->max_files; i > 1u; i--) {
        if (path_with_suffix(&source, file->path, i - 1u) &&
            path_with_suffix(&target, file->path, i)) {
            (void)rename(nb_buf_cstr(&source), nb_buf_cstr(&target));
        }
    }
    if (path_with_suffix(&target, file->path, 1u)) {
        (void)rename(file->path, nb_buf_cstr(&target));
    }
    nb_buf_destroy(&source);
    nb_buf_destroy(&target);

    file->size = 0;
    (void)open_stream(file);
}

nb_log_file *nb_log_file_open(const char *path, size_t max_bytes, unsigned max_files)
{
    if (!path || path[0] == '\0' || (max_bytes != 0 && max_files == 0)) {
        return NULL;
    }
    nb_log_file *file = nb_try_alloc(sizeof(*file));
    if (!file) {
        return NULL;
    }
    memset(file, 0, sizeof(*file));
    file->path = duplicate_cstr(path);
    file->mutex = nb_mutex_new(0);
    if (!file->path || !file->mutex) {
        nb_mutex_free(file->mutex);
        nb_free(file->path, file->path ? strlen(file->path) + 1u : 0u);
        nb_free(file, sizeof(*file));
        return NULL;
    }
    file->max_bytes = max_bytes;
    file->max_files = max_files;
    if (!open_stream(file)) {
        nb_mutex_free(file->mutex);
        nb_free(file->path, strlen(file->path) + 1u);
        nb_free(file, sizeof(*file));
        return NULL;
    }
    return file;
}

void nb_log_file_close(nb_log_file *file)
{
    if (!file) {
        return;
    }
    if (file->stream) {
        (void)fclose(file->stream);
    }
    nb_mutex_free(file->mutex);
    nb_free(file->path, strlen(file->path) + 1u);
    nb_free(file, sizeof(*file));
}

void nb_log_file_sink(void *ctx, nb_log_level level, const char *record, size_t len)
{
    (void)level;
    nb_log_file *file = ctx;
    if (!file || !record || len == 0) {
        return;
    }

    nb_mutex_lock(file->mutex);
    if (file->max_bytes != 0 && file->size != 0 &&
        (len > SIZE_MAX - file->size || file->size + len > file->max_bytes)) {
        rotate(file);
    }
    if (!file->stream) {
        (void)open_stream(file);
    }
    if (file->stream) {
        size_t written = fwrite(record, 1, len, file->stream);
        (void)fflush(file->stream);
        file->size += written;
    }
    nb_mutex_unlock(file->mutex);
}
