#ifndef MORPH_CLI_PRESENTATION_REWIND_H
#define MORPH_CLI_PRESENTATION_REWIND_H

#include "cJSON.h"
struct cli_application;

/* Borrows the report for rendering, including failed-restore recovery details. */
void cli_rewind_report_print(struct cli_application *ctx, const cJSON *report);

#endif
