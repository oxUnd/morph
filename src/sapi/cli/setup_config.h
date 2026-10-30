#ifndef MORPH_CLI_SETUP_CONFIG_H
#define MORPH_CLI_SETUP_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Merge setup-owned fields while retaining all other configuration values. */
int cli_setup_config_merge(const char *old, const char *patch, char **out);
int cli_setup_config_role(const char *text);

#ifdef __cplusplus
}
#endif
#endif
