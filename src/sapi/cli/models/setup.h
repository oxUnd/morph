#ifndef MORPH_CLI_MODELS_SETUP_H
#define MORPH_CLI_MODELS_SETUP_H

#include "config/config.h"

enum cli_setup_role_id {
	CLI_SETUP_GENERAL,
	CLI_SETUP_DEVELOPER,
	CLI_SETUP_PHOTOGRAPHER,
	CLI_SETUP_PHOTO_EDITOR,
	CLI_SETUP_WRITER,
	CLI_SETUP_DESIGNER,
	CLI_SETUP_VIDEO_CREATOR,
	CLI_SETUP_RESEARCHER,
	CLI_SETUP_ROLE_COUNT
};

struct cli_setup_role {
	const char *id;
	const char *label;
	const char *description;
	unsigned required_models;
};

#define CLI_SETUP_TOKEN_INPUT_MAX 32

enum cli_setup_model_kind {
	CLI_SETUP_TEXT,
	CLI_SETUP_VISION,
	CLI_SETUP_IMAGE,
	CLI_SETUP_VIDEO,
	CLI_SETUP_MODEL_COUNT
};

struct cli_setup_state {
	struct config_model_entry entries[CLI_SETUP_MODEL_COUNT];
	int role;
	int existing_config;
	int existing_prompt;
	int replace_prompt;
	int reconfigure;
};

#endif
