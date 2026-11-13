#include "sapi/cli/adapters/runtime/events.h"
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/services/agents.h"
#include "sapi/cli/pages/subagents.h"
#include "sapi/cli/application_internal.h"

int cli_runtime_events_callback(const struct morph_event *ev, void *user_data)
{
	struct cli_application *ctx = user_data;

	if (!ctx || !ev)
		return -EINVAL;
	if (ctx->events) {
		int rc;

		if (!cli_event_queue_is_owner(ctx) || !cli_event_queue_events_enabled(ctx))
			return cli_event_queue_post_morph_event(ctx, ev);
		rc = cli_event_queue_drain(ctx);
		if (rc != 0)
			return rc;
	}
	return cli_runtime_events_deliver(ctx, ev);
}

int cli_runtime_events_deliver(struct cli_application *ctx, const struct morph_event *ev)
{
	if (!ctx || !ev)
		return -EINVAL;
	if (ev->name && !strncmp(ev->name, "agent.", 6))
		(void)cli_agents_refresh(ctx);
	int subagent_rc = cli_subagents_event(ctx, ev);
	if (subagent_rc)
		return subagent_rc < 0 ? subagent_rc : 0;
	return cli_presentation_event(ctx, ev);
}
