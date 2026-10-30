#ifndef MORPH_CLI_SETUP_UI_H
#define MORPH_CLI_SETUP_UI_H

struct cli_context;
#include "config/config.h"
#include "setup_role.h"
#include <stdio.h>

#define CLI_SETUP_TOKEN_INPUT_MAX 32

enum cli_setup_model_kind {
	SETUP_TEXT,
	SETUP_VISION,
	SETUP_IMAGE,
	SETUP_VIDEO,
	SETUP_MODEL_COUNT
};

struct cli_setup_state {
	struct config_model_entry entries[SETUP_MODEL_COUNT];
	int role;
	int existing_config;
	int existing_prompt;
	int replace_prompt;
	int reconfigure;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Shared provider defaults for the terminal wizard and plain fallback. */
void cli_setup_model_defaults(int kind, int selected,
			      struct config_model_entry *entry);
int cli_setup_thinking_enabled(const struct config_model_entry *entry);
int cli_setup_set_thinking(struct config_model_entry *entry, int enabled);
void cli_setup_reset_token_limits(struct config_model_entry *entry);
int cli_setup_token_ceiling(const struct config_model_entry *entry, int output);
int cli_setup_parse_token_count(const char *text, int minimum, int maximum, int *value);
int cli_setup_provider_choice(int kind, const struct config_model_entry *entry);
const char *cli_setup_model_label(int kind);
void cli_setup_prepare_model(struct cli_setup_state *state, int kind);
void cli_setup_role_defaults(struct cli_setup_state *state);
void cli_setup_share_credentials(struct cli_setup_state *state, int kind,
				 const char *previous_env);
int cli_setup_model_complete(int kind, const struct config_model_entry *entry);
int cli_setup_ui_available(FILE *input, FILE *output);
/* Terminal receipts share the wizard margin; plain output keeps its format. */
int cli_setup_print(FILE *output, const char *format, ...);
int cli_setup_ui_collect(struct cli_context *ctx, const char *path, FILE *input, FILE *output,
			 struct cli_setup_state *state);

#ifdef __cplusplus
}
#endif

#endif
