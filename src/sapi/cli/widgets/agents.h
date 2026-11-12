#ifndef MORPH_CLI_WIDGETS_AGENTS_H
#define MORPH_CLI_WIDGETS_AGENTS_H

#include "agent/agent_control.h"
const char *cli_agents_status(enum agent_status status);
int cli_agents_receipt_append(morph_buf_t *out, const char *id, enum agent_status status,
			     int columns, int styled);
int cli_agents_detail(morph_buf_t *out, const char *label, const char *value, int columns,
		      int styled);
int cli_agents_summary(morph_buf_t *out, const struct agent_info *agents, int count, int columns);
int cli_agents_rows(morph_buf_t *out, const struct agent_info *agents, int count, int columns,
		    int max_agents, int styled);

#endif
