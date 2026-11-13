#include "sapi/cli/models/composer.h"
#include "nb/err.h"
#include "nb/cleanup.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
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
	rc = (nb_vec_init(&composer->images, sizeof(image)),
	      nb_vec_reserve(&composer->images, 16) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	return 0;
}

void cli_composer_model_cleanup(struct cli_composer_model *composer)
{
	struct cli_composer_model_input_image *image;

	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&composer->images) &&
	     (image = (struct cli_composer_model_input_image *)nb_vec_at(&composer->images, _i_));
	     _i_++)
		free(image->path);
	nb_vec_cleanup(&composer->images);
}

int cli_composer_model_add_image_resolved(struct cli_composer_model *composer,
					  const char *absolute_path, int width, int height,
					  const char **label)
{
	struct cli_composer_model_input_image *image;
	char *path;

	if (!composer || !absolute_path || !absolute_path[0] || !label)
		return NB_ERR_INVAL;
	if (width <= 0 || height <= 0)
		return NB_ERR_INVAL;
	if (composer->next_image == SIZE_MAX) {
		return NB_ERR_RANGE;
	}
	path = strdup(absolute_path);
	if (!path)
		return NB_ERR_NOMEM;
	image = nb_vec_push(&composer->images);
	if (!image) {
		free(path);
		return NB_ERR_NOMEM;
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

	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&composer->images) &&
	     (image = (struct cli_composer_model_input_image *)nb_vec_at(&composer->images, _i_));
	     _i_++) {
		if (strstr(input, image->label))
			visit(data, image->width, image->height);
	}
}

char *cli_composer_model_expand_copy(struct cli_composer_model *composer, const char *input)
{
	nb_buf result NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *cursor = input;

	if ((nb_buf_init(&result), nb_buf_reserve(&result, strlen(input) + 1) ? 0 : NB_ERR_NOMEM) !=
	    0)
		return NULL;
	while (*cursor) {
		struct cli_composer_model_input_image *image;
		struct cli_composer_model_input_image *matched = NULL;
		const char *next = NULL;

		for (size_t _i_ = 0; _i_ < nb_vec_len(&composer->images) &&
				     (image = (struct cli_composer_model_input_image *)nb_vec_at(
					      &composer->images, _i_));
		     _i_++) {
			const char *found = strstr(cursor, image->label);

			if (found && (!next || found < next)) {
				next = found;
				matched = image;
			}
		}
		if (!matched) {
			(void)nb_buf_append_cstr(&result, cursor);
			break;
		}
		(void)nb_buf_append(&result, cursor, (size_t)(next - cursor));
		(void)nb_buf_printf(&result, "%s [Image: %s]", matched->label, matched->path);
		cursor = next + strlen(matched->label);
	}
	if (nb_buf_status(&result)) {
		nb_buf_cleanup(&result);
		return NULL;
	}
	return (nb_buf_ok(&result) ? strdup(nb_buf_cstr(&result)) : NULL);
}

int cli_composer_model_image_span(struct cli_composer_model *composer, const char *input, int point,
				  int backward, int *start, int *end)
{
	struct cli_composer_model_input_image *image;

	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&composer->images) &&
	     (image = (struct cli_composer_model_input_image *)nb_vec_at(&composer->images, _i_));
	     _i_++) {
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
