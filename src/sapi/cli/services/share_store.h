#ifndef MORPH_CLI_SERVICES_SHARE_STORE_H
#define MORPH_CLI_SERVICES_SHARE_STORE_H
#include "util/json.h"
int cli_share_store_save(const struct json *document, char **out_path);
#endif
