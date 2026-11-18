#ifndef MORPH_CLI_PRESENTATION_PLAN_H
#define MORPH_CLI_PRESENTATION_PLAN_H

#include <nb/buf.h>
#include "cJSON.h"

struct cli_plan_style {
	int columns;
	int styled;
	int status_heading;
};

int cli_plan_renders(const cJSON *state);
int cli_plan_append(nb_buf *out, const cJSON *state, const struct cli_plan_style *style);

#endif
