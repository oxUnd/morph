#ifndef MORPH_CLI_PRESENTATION_AGENTS_H
#define MORPH_CLI_PRESENTATION_AGENTS_H

#include "event/event.h"
#include "agent/agent_control.h"
#include "sapi/cli/models/agents.h"

struct cli_application;

int cli_presentation_agents_event(struct cli_application *ctx, const struct morph_event *event);
void cli_presentation_agents_flush(struct cli_application *ctx);

const char *cli_agents_status(enum agent_status status);
int cli_agents_receipt_append(nb_buf *out, const char *id, enum agent_status status, int columns,
			      int styled);
int cli_agents_detail(nb_buf *out, const char *label, const char *value, int columns, int styled);
int cli_agents_summary(nb_buf *out, const struct agent_info *agents, int count, int columns);
int cli_agents_rows(nb_buf *out, const struct agent_info *agents, int count, int columns,
		    int max_agents, int styled);
int cli_agents_model_render(const struct cli_agents_model *model, nb_buf *out, nb_buf *summary,
			    int columns, int maximum, int styled);

#endif
