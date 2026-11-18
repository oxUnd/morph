#ifndef MORPH_CLI_COMPONENTS_PLAN_H
#define MORPH_CLI_COMPONENTS_PLAN_H

#include <nothing/tui/view.h>
#include "cJSON.h"

struct nt_view *cli_plan_add_borrow(struct nt_builder *, struct nt_view *parent, const cJSON *state,
				    int styled);

#endif
