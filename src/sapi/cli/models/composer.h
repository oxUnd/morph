#ifndef MORPH_CLI_MODELS_COMPOSER_H
#define MORPH_CLI_MODELS_COMPOSER_H

#include "util/array.h"

struct cli_composer_model {
	morph_array_t images;
	size_t next_image;
};

int cli_composer_model_init(struct cli_composer_model *composer);
void cli_composer_model_cleanup(struct cli_composer_model *composer);
int cli_composer_model_add_image(struct cli_composer_model *composer, const char *path,
				 const char **label);
int cli_composer_model_convert_paths(struct cli_composer_model *composer, const char *input,
				     char **output);
void cli_composer_model_visit_images(struct cli_composer_model *composer, const char *input,
				     void (*visit)(void *, int, int), void *data);
char *cli_composer_model_expand(struct cli_composer_model *composer, const char *input);
int cli_composer_model_image_span(struct cli_composer_model *composer, const char *input, int point,
				  int backward, int *start, int *end);

#endif
