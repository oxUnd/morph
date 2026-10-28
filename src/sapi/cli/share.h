#ifndef MORPH_CLI_SHARE_H
#define MORPH_CLI_SHARE_H

#include "cJSON.h"
#include "util/buf.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

int cli_share_html(const cJSON *document, time_t shared_at, morph_buf_t *html);
int cli_share_save(const cJSON *document, char **out_path);

#ifdef __cplusplus
}
#endif
#endif
