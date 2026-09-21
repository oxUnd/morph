#ifndef MORPH_CLI_SETUP_UI_H
#define MORPH_CLI_SETUP_UI_H

#include "config/config.h"
#include <stdio.h>

#define CLI_SETUP_TOKEN_INPUT_MAX 32

enum cli_setup_model_kind {
	SETUP_TEXT,
	SETUP_VISION,
	SETUP_IMAGE,
	SETUP_VIDEO,
	SETUP_MODEL_COUNT
};

#ifdef __cplusplus
extern "C" {
#endif

/* Shared provider defaults for the terminal wizard and plain fallback. */
void cli_setup_model_defaults(int kind, int selected,
			      struct config_model_entry *entry);
void cli_setup_reset_token_limits(struct config_model_entry *entry);
int cli_setup_token_ceiling(const struct config_model_entry *entry, int output);
int cli_setup_parse_token_count(const char *text, int minimum, int maximum, int *value);
int cli_setup_ui_available(FILE *input, FILE *output);
int cli_setup_ui_collect(const char *path, FILE *input, FILE *output,
			 struct config_model_entry entries[SETUP_MODEL_COUNT]);

#ifdef __cplusplus
}
#endif

#endif
