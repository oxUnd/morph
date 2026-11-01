#include "sapi/cli/ui/runtime_events.h"
#include "sapi/cli/ui/presentation/presenter.h"
#include "sapi/cli/ui/event_queue.h"
#include "sapi/cli/internal.h"

int cli_runtime_events_callback(const struct morph_event *ev, void *user_data)
{
	struct cli_context *ctx = user_data;

	if (!ctx || !ev)
		return -EINVAL;
	if (ctx->ui) {
		int rc;

		if (!cli_event_queue_is_owner(ctx))
			return cli_event_queue_post_morph_event(ctx, ev);
		rc = cli_event_queue_drain(ctx);
		if (rc != 0)
			return rc;
	}
	return cli_presentation_event(ctx, ev);
}
