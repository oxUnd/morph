#ifndef MORPH_CLI_SERVICES_AGENTS_H
#define MORPH_CLI_SERVICES_AGENTS_H

#include "agent/agent_control.h"
#include <stdint.h>

struct cli_application;
struct agent_info;

/* Refresh the application's agent display model from the runtime facade. */
int cli_agents_refresh(struct cli_application *ctx);

int cli_agents_list(struct cli_application *ctx, int64_t session, struct agent_info **items,
		    int *count);
int cli_agents_activity(struct cli_application *ctx, int64_t session, struct agent_info **items,
			int *count);
int cli_agents_read(struct cli_application *ctx, const char *id, struct agent_info **out);
int cli_agents_subscribe(struct cli_application *ctx, const char *id, char ***events,
			 int *count, int64_t *sequence);
int cli_agents_select(struct cli_application *ctx, const char *id);
int cli_agents_followup(struct cli_application *ctx, const char *id, const char *message);
int cli_agents_interrupt(struct cli_application *ctx, const char *id);
void cli_agents_list_free(struct agent_info *items, int count);
void cli_agents_events_free(char **items, int count);

#endif
