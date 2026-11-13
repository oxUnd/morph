#ifndef MORPH_CLI_PRESENTATION_SHARE_H
#define MORPH_CLI_PRESENTATION_SHARE_H

#include "cJSON.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

int cli_share_html(const cJSON *document, time_t shared_at, nb_buf *html);

#ifdef __cplusplus
}
#endif
#endif
