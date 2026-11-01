#ifndef MORPH_CLI_SERVICES_CONFIGURATION_MODEL_H
#define MORPH_CLI_SERVICES_CONFIGURATION_MODEL_H

#include "sapi/cli/models/setup.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared provider defaults for the terminal wizard and plain fallback. */
void cli_setup_model_defaults(int kind, int selected, struct config_model_entry *entry);
int cli_setup_thinking_enabled(const struct config_model_entry *entry);
int cli_setup_set_thinking(struct config_model_entry *entry, int enabled);
void cli_setup_reset_token_limits(struct config_model_entry *entry);
int cli_setup_token_ceiling(const struct config_model_entry *entry, int output);
int cli_setup_parse_token_count(const char *text, int minimum, int maximum, int *value);
int cli_setup_provider_choice(int kind, const struct config_model_entry *entry);
const char *cli_setup_model_label(int kind);
void cli_setup_prepare_model(struct cli_setup_state *state, int kind);
void cli_setup_role_defaults(struct cli_setup_state *state);
void cli_setup_share_credentials(struct cli_setup_state *state, int kind, const char *previous_env);
int cli_setup_model_complete(int kind, const struct config_model_entry *entry);

#ifdef __cplusplus
}
#endif
#endif
