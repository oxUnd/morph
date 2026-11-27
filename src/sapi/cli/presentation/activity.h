#ifndef MORPH_CLI_PRESENTATION_ACTIVITY_H
#define MORPH_CLI_PRESENTATION_ACTIVITY_H

#include <nb/buf.h>
#include <stddef.h>
#include <stdint.h>

struct cli_activity_snapshot {
	int agents, jobs, mcp_connecting, other_sessions;
	int session_titles_pending, recap_pending;
	int64_t tasks_running, tasks_pending;
	size_t queued;
};

int cli_activity_auxiliary_render(const struct cli_activity_snapshot *, int columns, int styled,
	nb_buf *tasks, nb_buf *auxiliary, nb_buf *compact);

#endif
