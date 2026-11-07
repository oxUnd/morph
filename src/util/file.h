#ifndef FILE_H
#define FILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* Independent malloc-owned bytes, NUL-terminated; release with free(). */
char *file_read_all_copy(const char *path, size_t *out_len);
/* Atomic regular-file replacement. Borrows path/data; preserves existing mode,
 * creates new files as 0600 and rejects symlinks. On a post-rename sync failure,
 * returns an error although the new file may already be visible. */
int file_write_atomic(const char *path, const char *data, size_t len);
int file_write_all(const char *path, const char *data, size_t len);
int file_ensure_dir(const char *path);
int file_exists(const char *path);
/* Independent expanded path; caller releases it with free(). */
char *file_expand_path_copy(const char *path);
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
/* Independent names and array; release with file_free_list().
 * Output slots must be empty; failures leave no partial list. */
int file_list_dirs_copy(const char *dir, char ***out_names, int *out_count);
int file_list_files_copy(const char *dir, char ***out_names, int *out_count);
void file_free_list(char **list, int count);
/* Independent canonical path; caller releases it with free(). */
char *file_resolve_path_copy(const char *path);
int path_is_within(const char *path, const char *dir);

#ifdef __cplusplus
}
#endif

#endif
