#ifndef IMAGE_GEN_H
#define IMAGE_GEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <stddef.h>

struct model;

struct image_result {
	char path[PATH_MAX];
	char url[PATH_MAX];
	int width;
	int height;
};

const char *image_gen_ext_from_content_type(const char *headers);
const char *image_gen_ext_from_magic(const unsigned char *data, size_t len);

int image_gen_create(struct model *self, const char *prompt, const char *style,
		    const char *size, const char *image_path,
		    const char *output_dir,
		    struct image_result *result);

#ifdef __cplusplus
}
#endif

#endif
