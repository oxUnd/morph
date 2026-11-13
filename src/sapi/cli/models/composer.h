#ifndef MORPH_CLI_MODELS_COMPOSER_H
#define MORPH_CLI_MODELS_COMPOSER_H

#include "util/array.h"
#include <stdint.h>

struct cli_composer_model {
	morph_array_t images;
	size_t next_image;
	int ultra;
	int mode_ready;
	int mode_transition;
	int64_t mode_started_ms;
};

int cli_composer_model_init(struct cli_composer_model *composer);
void cli_composer_model_cleanup(struct cli_composer_model *composer);
/* The model only stores already resolved input: `absolute_path` must be an
 * existing absolute path and width/height must come from image probing.
 * Copies path. The returned label borrows an array slot until the next image
 * addition (which can reallocate) or model cleanup. */
int cli_composer_model_add_image_resolved(struct cli_composer_model *composer,
					  const char *absolute_path, int width, int height,
					  const char **label_borrow);
void cli_composer_model_visit_images(struct cli_composer_model *composer, const char *input,
				     void (*visit)(void *, int, int), void *data);
char *cli_composer_model_expand_copy(struct cli_composer_model *composer, const char *input);
int cli_composer_model_image_span(struct cli_composer_model *composer, const char *input, int point,
				  int backward, int *start, int *end);

/* UI owner supplies the clock; initial observation and repeated selection do not animate. */
void cli_composer_model_set_ultra(struct cli_composer_model *composer, int ultra, int animate);
int cli_composer_model_prompt_width(const struct cli_composer_model *composer);
/* -1 when idle; 0..30 for the wave, including its final clear frame. */
int cli_composer_model_animation_frame(struct cli_composer_model *composer, int64_t now_ms);

#endif
