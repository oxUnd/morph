#ifndef MORPH_CLI_PRESENTATION_RECAP_H
#define MORPH_CLI_PRESENTATION_RECAP_H

struct cli_application;

/* Register recap result/failure rendering with the recap service. */
void cli_presentation_recap_attach(struct cli_application *ctx);

#endif
