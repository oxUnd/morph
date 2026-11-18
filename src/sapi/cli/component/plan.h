#ifndef MORPH_CLI_COMPONENT_PLAN_H
#define MORPH_CLI_COMPONENT_PLAN_H

#include <nb/buf.h>
#include <nb/cleanup.h>
#include "cJSON.h"
#include <nothing/tui/view.h>

struct cli_plan_style {
	int columns;
	int styled;
	int status_heading;
};

int cli_plan_renders(const cJSON *state);
int cli_plan_append(nb_buf *out, const cJSON *state, const struct cli_plan_style *style);
struct nt_view *cli_plan_add_borrow(struct nt_builder *,
	struct nt_view *parent, const cJSON *state, int styled);

#endif
