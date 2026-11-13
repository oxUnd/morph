#ifndef MORPH_CLI_PRESENTATION_COMPOSER_FOOTER_H
#define MORPH_CLI_PRESENTATION_COMPOSER_FOOTER_H

#include "nb/buf.h"
#include "nb/cleanup.h"

struct cli_application;

/* Trusted SGR content using the original adaptive layout. columns is the full
 * terminal width; the component supplies left/right padding of two cells. */
int cli_composer_footer_render(struct cli_application *, int columns, nb_buf *out);

#endif
