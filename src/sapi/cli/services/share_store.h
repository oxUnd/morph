#ifndef MORPH_CLI_SERVICES_SHARE_STORE_H
#define MORPH_CLI_SERVICES_SHARE_STORE_H
#include "cJSON.h"
/* Writes the document and returns an independent malloc-owned path.
 * out_path must be NULL; failure preserves it. Release with free(). */
int cli_share_store_save(const cJSON *document, char **out_path);
#endif
