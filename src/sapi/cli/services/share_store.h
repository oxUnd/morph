#ifndef MORPH_CLI_SERVICES_SHARE_STORE_H
#define MORPH_CLI_SERVICES_SHARE_STORE_H
#include "cJSON.h"
int cli_share_store_save(const cJSON *document, char **out_path);
#endif
