#ifndef MORPH_DATA_H
#define MORPH_DATA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/*
 * Resolve a path relative to a morph data root.
 *
 * Data lives under ../share/morph relative to the running executable.
 */
int morph_data_find(char *dst, size_t dst_size, const char *relative_path);
char *morph_data_find_alloc(const char *relative_path);
int morph_executable_find(char *dst, size_t dst_size, const char *name);

#ifdef __cplusplus
}
#endif

#endif
