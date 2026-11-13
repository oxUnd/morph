#ifndef MORPH_CLI_WIDGETS_AGENTS_H
#define MORPH_CLI_WIDGETS_AGENTS_H

#include "agent/agent_control.h"
#include "sapi/cli/models/agents.h"
const char *cli_agents_status(enum agent_status status);
int cli_agents_receipt_append(morph_buf_t *out, const char *id, enum agent_status status,
			     int columns, int styled);
int cli_agents_detail(morph_buf_t *out, const char *label, const char *value, int columns,
		      int styled);
int cli_agents_summary(morph_buf_t *out, const struct agent_info *agents, int count, int columns);
int cli_agents_rows(morph_buf_t *out, const struct agent_info *agents, int count, int columns,
		    int max_agents, int styled);
int cli_agents_model_visible(const struct cli_agents_model *model);
int cli_agents_model_render(const struct cli_agents_model *model, morph_buf_t *out,
			    morph_buf_t *summary, int columns, int maximum, int styled);

#endif
