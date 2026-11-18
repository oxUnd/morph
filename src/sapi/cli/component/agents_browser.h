#ifndef MORPH_CLI_COMPONENT_AGENTS_BROWSER_H
#define MORPH_CLI_COMPONENT_AGENTS_BROWSER_H

#include "agent/agent_control.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include <nothing/tui/models/list_navigation.h>

enum cli_agents_browser_filter {
	CLI_AGENTS_BROWSER_ALL,
	CLI_AGENTS_BROWSER_RUNNING,
	CLI_AGENTS_BROWSER_PENDING,
	CLI_AGENTS_BROWSER_COMPLETED,
	CLI_AGENTS_BROWSER_FAILED,
	CLI_AGENTS_BROWSER_INTERRUPTED,
	CLI_AGENTS_BROWSER_INACTIVE,
	CLI_AGENTS_BROWSER_FILTER_COUNT,
};

struct cli_agents_browser_state {
	struct nt_list_state selection;
	enum cli_agents_browser_filter filter;
	size_t detail_scroll;
	int detail_focus;
	int help;
};

/* The retained browser borrows the snapshot fields through its next sync or destruction. */
struct cli_agents_browser_data {
	const struct agent_info *agents;
	int count;
	const struct agent_info *detail;
	const char *project;
	const char *model;
	int root_running;
	int64_t now_ms;
};

/* Original snapshot index: zero is the main conversation, n+1 is agents[n].
 * SIZE_MAX means the active filter has no matching entries. */
size_t cli_agents_browser_selected(const struct cli_agents_browser_state *state,
				   const struct cli_agents_browser_data *data);
void cli_agents_browser_select(struct cli_agents_browser_state *state,
			       const struct cli_agents_browser_data *data, size_t index);
int cli_agents_browser_update(struct cli_agents_browser_state *state,
			      const struct cli_agents_browser_data *data, int key);
struct nt_builder;
struct nt_view;
struct nt_view *cli_agents_browser_add(struct nt_builder *, struct nt_view *parent);
int cli_agents_browser_sync(struct nt_view *, struct cli_agents_browser_state *,
			    const struct cli_agents_browser_data *, int styled);
#endif
