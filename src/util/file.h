#ifndef FILE_H
#define FILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

char *file_read_all(const char *path, size_t *out_len);
int file_write_all(const char *path, const char *data, size_t len);
int file_ensure_dir(const char *path);
int file_exists(const char *path);
char *file_expand_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif