#ifndef MORPH_CONFIG_EDIT_H
#define MORPH_CONFIG_EDIT_H
#ifdef __cplusplus
extern "C" {
#endif
/* Inputs are borrowed for the call. out must be NULL; success returns an
 * independent malloc owner, and failure preserves out. Release with free(). */
int config_text_merge_copy(const char *old, const char *patch, char **out);
int config_text_credentials_copy(const char *text, char **out);
int config_text_mcp_add_copy(const char *text, const char *server, char **out);
#ifdef __cplusplus
}
#endif
#endif
