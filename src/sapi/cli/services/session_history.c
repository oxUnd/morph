#include "sapi/cli/services/session_history.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/history.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/widgets/feed.h"
#include "runtime/runtime.h"
#include "util/error.h"

int cli_session_history_restore(struct cli_application *ctx)
{
	struct model_history_item *items;
	struct message *messages = NULL;
	int count = 0;
	int rc;

	if (!ctx || !ctx->runtime)
		return -EINVAL;
	/* Session commands retain their existing plain and NDJSON contracts. */
	if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return 0;
	items = runtime_session_model_history_current(ctx->runtime, 0, &count);
	if (!items)
		messages = runtime_session_messages_current(ctx->runtime, &count);
	cli_feed_history_begin(ctx);
	cli_presentation_reset(ctx);
	rc = cli_history_render_session(ctx, items, messages);
	runtime_session_model_history_free(items);
	runtime_session_messages_free(messages);
	cli_feed_history_end(ctx);
	return rc;
}
