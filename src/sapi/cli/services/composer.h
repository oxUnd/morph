#ifndef MORPH_CLI_SERVICES_COMPOSER_H
#define MORPH_CLI_SERVICES_COMPOSER_H

struct cli_composer_model;

/* Resolve a user supplied path (shell expansion, symlinks), verify that it is
 * a readable image and probe its dimensions before handing the resolved
 * absolute path to the model. The model owns a copy of that path. */
int cli_composer_service_add_image(struct cli_composer_model *composer, const char *path,
				   const char **label_borrow);

/* Preserve source spans while decoding dragged paths (quotes and escaped
 * spaces). Ordinary text, including its whitespace, remains byte-for-byte.
 * Returns the number of converted paths or a negative error code. */
int cli_composer_service_convert_paths_copy(struct cli_composer_model *composer,
					    const char *input, char **output);

#endif
