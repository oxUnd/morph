#include "sapi/cli/presentation/activity.h"

#include "sapi/cli/presentation/work_status.h"
#include <nb/cleanup.h>
#include <nb/err.h>

int cli_activity_auxiliary_render(const struct cli_activity_snapshot *snapshot,
	int columns, int styled, nb_buf *tasks, nb_buf *auxiliary, nb_buf *compact)
{
	if (!snapshot || !tasks || !auxiliary || !compact)
		return NB_ERR_INVAL;

	nb_buf label NB_CLEANUP(nb_buf_cleanup) = {0};
	if ((snapshot->tasks_running || snapshot->tasks_pending) &&
	    !nb_buf_printf(&label, "Tasks %lld running · %lld pending",
		(long long)snapshot->tasks_running, (long long)snapshot->tasks_pending))
		return nb_buf_status(&label);

	int rc = label.len ? cli_work_status_detail(tasks, nb_buf_cstr(&label), columns, styled) : 0;
	if (!rc && snapshot->session_titles_pending > 0)
		rc = cli_work_status_detail(auxiliary, "Naming session...", columns, styled);

	if (!rc && snapshot->recap_pending)
		rc = cli_work_status_detail(auxiliary, "Generating recap...", columns, styled);

	nb_buf_clear(&label);
	if (!rc && snapshot->mcp_connecting &&
	    !nb_buf_printf(&label, "MCP · %d connecting · /mcp", snapshot->mcp_connecting))
		rc = nb_buf_status(&label);

	if (!rc && label.len)
		rc = cli_work_status_detail(auxiliary, nb_buf_cstr(&label), columns, styled);

	if (!rc && snapshot->other_sessions > 0) {
		nb_buf_clear(&label);
		if (!nb_buf_printf(&label, "%d other session%s running", snapshot->other_sessions,
			snapshot->other_sessions == 1 ? "" : "s"))
			rc = nb_buf_status(&label);

		if (!rc)
			rc = cli_work_status_detail(auxiliary, nb_buf_cstr(&label), columns, styled);
	}

	nb_buf_clear(&label);
	if (!rc && !nb_buf_printf(&label, "Agents %d · Jobs %d · Tasks %lld · Queue %zu",
		snapshot->agents, snapshot->jobs,
		(long long)(snapshot->tasks_running + snapshot->tasks_pending), snapshot->queued))
		rc = nb_buf_status(&label);

	return rc ? rc : cli_work_status_detail(compact, nb_buf_cstr(&label), columns, styled);
}
