#include "sapi/cli/services/agents.h"
#include "nb/err.h"
#include "sapi/cli/models/agents.h"

#include "sapi/cli/application.h"
#include "sapi/cli/models/jobs.h"
#include "runtime/runtime.h"
#include <errno.h>

int cli_agents_refresh(struct cli_application *ctx)
{
	struct agent_info *items = NULL;
	int count = 0;
	int64_t session = 0;
	int rc;

	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;
	rc = runtime_session_current_id(ctx->runtime, &session);
	if (rc != 0)
		return rc;
	rc = runtime_agent_activity(ctx->runtime, session, &items, &count);
	if (rc != 0)
		return rc;
	cli_agents_model_set(&ctx->agents, items, count, session);
	cli_jobs_model_set_session(ctx->jobs, session);
	return 0;
}

int cli_agents_list(struct cli_application *ctx, int64_t session, struct agent_info **items,
		    int *count)
{
	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;
	return runtime_agent_list(ctx->runtime, session, items, count);
}

int cli_agents_activity(struct cli_application *ctx, int64_t session, struct agent_info **items,
			int *count)
{
	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;
	return runtime_agent_activity(ctx->runtime, session, items, count);
}

int cli_agents_read(struct cli_application *ctx, const char *id, struct agent_info **out)
{
	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;
	return runtime_agent_read(ctx->runtime, id, out);
}

int cli_agents_subscribe(struct cli_application *ctx, const char *id, char ***events,
			 int *count, int64_t *sequence)
{
	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;
	return runtime_agent_subscribe(ctx->runtime, id, events, count, sequence);
}

int cli_agents_select(struct cli_application *ctx, const char *id)
{
	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;
	return runtime_agent_select(ctx->runtime, id);
}

int cli_agents_followup(struct cli_application *ctx, const char *id, const char *message)
{
	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;
	return runtime_agent_followup(ctx->runtime, id, message);
}

int cli_agents_interrupt(struct cli_application *ctx, const char *id)
{
	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;
	return runtime_agent_interrupt(ctx->runtime, id);
}

void cli_agents_list_free(struct agent_info *items, int count)
{
	runtime_agent_list_free(items, count);
}

void cli_agents_events_free(char **items, int count)
{
	runtime_agent_events_free(items, count);
}
