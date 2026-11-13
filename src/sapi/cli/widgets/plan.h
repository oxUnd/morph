#ifndef MORPH_CLI_WIDGETS_PLAN_H
#define MORPH_CLI_WIDGETS_PLAN_H

#include "util/buf.h"
#include "cJSON.h"

struct cli_plan_style {
	int columns;
	int styled;
	int status_heading;
};

int cli_plan_renders(const cJSON *state);
int cli_plan_append(morph_buf_t *out, const cJSON *state, const struct cli_plan_style *style);

#endif
