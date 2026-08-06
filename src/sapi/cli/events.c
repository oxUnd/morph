#include "sapi/cli/internal.h"

int cli_event_callback(const struct morph_event *ev, void *user_data)
{
	struct cli_context *ctx = user_data;

	if (!ctx || !ev)
		MORPH_RETURN(-EINVAL);
	if (ctx->ui) {
		int rc;

		if (!cli_ui_is_owner(ctx))
			return cli_ui_post_morph_event(ctx, ev);
		rc = cli_ui_drain(ctx);
		if (rc != 0)
			return rc;
	}
	return cli_presentation_event(ctx, ev);
}
