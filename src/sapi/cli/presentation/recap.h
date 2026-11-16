#ifndef MORPH_CLI_PRESENTATION_RECAP_H
#define MORPH_CLI_PRESENTATION_RECAP_H

#include <nb/buf.h>

struct cli_application;

/* Compact recap with a hanging indent. Text is copied and sanitized into out. */
int cli_recap_append(nb_buf *out, const char *summary, const char *next, int columns, int styled);

/* Register recap result/failure rendering with the recap service. */
void cli_presentation_recap_attach(struct cli_application *ctx);

#endif
