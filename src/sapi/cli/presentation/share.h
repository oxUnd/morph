#ifndef MORPH_CLI_PRESENTATION_SHARE_H
#define MORPH_CLI_PRESENTATION_SHARE_H

#include "cJSON.h"
#include "util/buf.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

int cli_share_html(const cJSON *document, time_t shared_at, morph_buf_t *html);

#ifdef __cplusplus
}
#endif
#endif
