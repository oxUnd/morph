#ifndef MORPH_CONFIG_EDIT_H
#define MORPH_CONFIG_EDIT_H
#ifdef __cplusplus
extern "C" {
#endif
int config_text_merge(const char *old, const char *patch, char **out);
int config_text_credentials(const char *text, char **out);
int config_text_mcp_add(const char *text, const char *server, char **out);
#ifdef __cplusplus
}
#endif
#endif
