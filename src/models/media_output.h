#ifndef MORPH_MEDIA_OUTPUT_H
#define MORPH_MEDIA_OUTPUT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Publish complete bytes under an exclusively created name. On failure no
 * output path is returned and the unpublished temporary file is removed. */
int media_output_write(const char *output_dir, const char *prefix, const char *extension,
		       const void *data, size_t len, char *path, size_t path_size);

#ifdef __cplusplus
}
#endif

#endif
