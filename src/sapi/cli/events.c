#include "sapi/cli/internal.h"

int cli_event_callback(const struct morph_event *ev, void *user_data)
{
	struct cli_context *ctx = user_data;

	if (!ctx || !ev)
		MORPH_RETURN(-EINVAL);
	return cli_presentation_event(ctx, ev);
}
