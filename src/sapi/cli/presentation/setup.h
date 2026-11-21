#ifndef MORPH_CLI_PRESENTATION_SETUP_H
#define MORPH_CLI_PRESENTATION_SETUP_H

#include <stdio.h>

/* Terminal receipts share the setup margin; plain output keeps its format. */
int cli_setup_print(FILE *output, const char *format, ...);

#endif
