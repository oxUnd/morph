#ifndef MORPH_CLI_PAGES_HOOKS_H
#define MORPH_CLI_PAGES_HOOKS_H

#include "cJSON.h"

struct cli_application;
struct nt_view_controller;

int cli_hooks_create_take(cJSON **rows, const char *review_id, int batch,
			  struct nt_view_controller **out);
int cli_hooks_startup(struct cli_application *ctx);

#endif
