#include "sapi/cli/models/composer.h"
#include "util/cleanup.h"
#include "util/buf.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLI_COMPOSER_MODEL_IMAGE_LABEL_MAX 64

struct cli_composer_model_input_image {
	char *path;
	int width;
	int height;
	char label[CLI_COMPOSER_MODEL_IMAGE_LABEL_MAX];
};

int cli_composer_model_init(struct cli_composer_model *composer)
{
	struct cli_composer_model_input_image image;
	int rc;

	memset(composer, 0, sizeof(*composer));
	rc = morph_array_init(&composer->images, MORPH_ARRAY_INIT_CAP, sizeof(image));
	if (rc != 0)
		return rc;
	return 0;
}

void cli_composer_model_cleanup(struct cli_composer_model *composer)
{
	struct cli_composer_model_input_image *image;

	morph_array_foreach (image, &composer->images, struct cli_composer_model_input_image)
		free(image->path);
	morph_array_cleanup(&composer->images);
}

int cli_composer_model_add_image_resolved(struct cli_composer_model *composer,
					  const char *absolute_path, int width, int height,
					  const char **label)
{
	struct cli_composer_model_input_image *image;
	char *path;

	if (!composer || !absolute_path || !absolute_path[0] || !label)
		return -EINVAL;
	if (width <= 0 || height <= 0)
		return -EINVAL;
	if (composer->next_image == SIZE_MAX) {
		return -EOVERFLOW;
	}
	path = strdup(absolute_path);
	if (!path)
		return -ENOMEM;
	image = morph_array_push_borrow(&composer->images);
	if (!image) {
		free(path);
		return -ENOMEM;
	}
	image->path = path;
	image->width = width;
	image->height = height;
	composer->next_image++;
	snprintf(image->label, sizeof(image->label), "[IMAGE#%zu]", composer->next_image);
	*label = image->label;
	return 0;
}

void cli_composer_model_visit_images(struct cli_composer_model *composer, const char *input,
				     void (*visit)(void *, int, int), void *data)
{
	struct cli_composer_model_input_image *image;

	morph_array_foreach (image, &composer->images, struct cli_composer_model_input_image) {
		if (strstr(input, image->label))
			visit(data, image->width, image->height);
	}
}

char *cli_composer_model_expand_copy(struct cli_composer_model *composer, const char *input)
{
	morph_buf_t result MORPH_CLEANUP(morph_buf_cleanup) = {0};
	const char *cursor = input;

	if (morph_buf_init(&result, strlen(input) + 1) != 0)
		return NULL;
	while (*cursor) {
		struct cli_composer_model_input_image *image;
		struct cli_composer_model_input_image *matched = NULL;
		const char *next = NULL;

		morph_array_foreach (image, &composer->images,
				     struct cli_composer_model_input_image) {
			const char *found = strstr(cursor, image->label);

			if (found && (!next || found < next)) {
				next = found;
				matched = image;
			}
		}
		if (!matched) {
			(void)morph_buf_puts(&result, cursor);
			break;
		}
		(void)morph_buf_append(&result, cursor, (size_t)(next - cursor));
		(void)morph_buf_printf(&result, "%s [Image: %s]", matched->label, matched->path);
		cursor = next + strlen(matched->label);
	}
	if (result.failed) {
		morph_buf_cleanup(&result);
		return NULL;
	}
	return morph_buf_detach(&result);
}

int cli_composer_model_image_span(struct cli_composer_model *composer, const char *input, int point,
				  int backward, int *start, int *end)
{
	struct cli_composer_model_input_image *image;

	morph_array_foreach (image, &composer->images, struct cli_composer_model_input_image) {
		const char *found = input;

		while ((found = strstr(found, image->label))) {
			int left = (int)(found - input);
			int right = left + (int)strlen(image->label);

			if (backward ? point > left && point <= right
				     : point >= left && point < right) {
				*start = left;
				*end = right;
				return 1;
			}
			found += strlen(image->label);
		}
	}
	return 0;
}

void cli_composer_model_set_ultra(struct cli_composer_model *composer, int ultra, int animate)
{
	ultra = !!ultra;
	if (composer->mode_ready && composer->ultra == ultra)
		return;
	composer->mode_transition = composer->mode_ready && animate && ultra;
	composer->mode_ready = 1;
	composer->ultra = ultra;
	composer->mode_started_ms = -1;
}

int cli_composer_model_prompt_width(const struct cli_composer_model *composer)
{
	(void)composer;
	return 2;
}

int cli_composer_model_animation_frame(struct cli_composer_model *composer, int64_t now_ms)
{
	if (!composer->mode_transition)
		return -1;
	if (composer->mode_started_ms < 0)
		composer->mode_started_ms = now_ms;
	int64_t elapsed = now_ms - composer->mode_started_ms;
	if (elapsed < 0)
		elapsed = 0;
	if (elapsed >= 990) {
		composer->mode_transition = 0;
		return 30;
	}
	return (int)(elapsed / 33);
}
