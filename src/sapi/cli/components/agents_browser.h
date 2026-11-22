#ifndef MORPH_CLI_COMPONENTS_AGENTS_BROWSER_H
#define MORPH_CLI_COMPONENTS_AGENTS_BROWSER_H

#include "agent/agent_control.h"
#include <nb/vec.h>

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
	size_t selected;
	nb_vec rows;
	size_t counts[CLI_AGENTS_BROWSER_FILTER_COUNT];
	size_t offsets[CLI_AGENTS_BROWSER_FILTER_COUNT];
	uint64_t revision;
	int root_running, live_elapsed;
	enum cli_agents_browser_filter filter;
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

/* Refresh before syncing a changed snapshot. Zero identifies the main conversation,
 * n+1 identifies agents[n]. The state owns its grouped row indices. */
int cli_agents_browser_set_snapshot(struct cli_agents_browser_state *state,
				    const struct cli_agents_browser_data *data, size_t selected);
void cli_agents_browser_state_cleanup(struct cli_agents_browser_state *state);
/* SIZE_MAX means the active filter has no matching entries. */
size_t cli_agents_browser_selected(const struct cli_agents_browser_state *state);
struct nt_builder;
struct nt_view;
struct nt_view *cli_agents_browser_add(struct nt_builder *, struct nt_view *parent,
				      struct nt_view *footer);
int cli_agents_browser_focus(struct nt_view *, int details);
int cli_agents_browser_sync(struct nt_view *, struct cli_agents_browser_state *,
			    const struct cli_agents_browser_data *, int styled);
#endif
