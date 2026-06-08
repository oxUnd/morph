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
int file_path_is_absolute(const char *path);
int file_path_full(char *dst, size_t dst_size,
		   const char *prefix, const char *path);
char *file_path_full_alloc(const char *prefix, const char *path);
int file_path_join(char *dst, size_t dst_size,
		   const char *dir, const char *name);
char *file_path_join_alloc(const char *dir, const char *name);
int file_path_append(char *dst, size_t dst_size, const char *suffix);
char *file_path_append_alloc(const char *path, const char *suffix);
int file_path_join_append(char *dst, size_t dst_size,
			  const char *dir, const char *name,
			  const char *suffix);
int file_list_dirs(const char *dir, char ***out_names, int *out_count);
int file_list_files(const char *dir, char ***out_names, int *out_count);
void file_free_list(char **list, int count);
char *file_resolve_path(const char *path);
int path_is_within(const char *path, const char *dir);

#ifdef __cplusplus
}
#endif

#endif
