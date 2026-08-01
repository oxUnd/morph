#ifndef IMAGE_GEN_H
#define IMAGE_GEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <stddef.h>

struct model;

#define IMAGE_GEN_MAX_REFERENCE_IMAGES 10

struct image_result {
	char path[PATH_MAX];
	char url[PATH_MAX];
	int width;
	int height;
};

const char *image_gen_ext_from_content_type(const char *headers);
const char *image_gen_ext_from_magic(const unsigned char *data, size_t len);
int image_gen_validate_size(const char *size);
int image_gen_validate_size_for_model(const struct model *model,
				      const char *size);
const char *image_gen_adapter_name(const struct model *model);
int image_gen_adapter_supported(const char *provider, const char *adapter);

int image_gen_create(struct model *self, const char *prompt, const char *style,
		    const char *size, const char **image_paths,
		    int image_count,
		    const char *output_dir,
		    struct image_result *result);

#ifdef __cplusplus
}
#endif

#endif
