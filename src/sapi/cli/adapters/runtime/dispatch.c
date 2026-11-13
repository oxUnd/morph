#include "sapi/cli/adapters/runtime/dispatch.h"
#include "sapi/cli/adapters/runtime/events.h"
#include "sapi/cli/application.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/services/event_queue_item.h"
#include "sapi/cli/adapters/input/cancel.h"
#include "sapi/cli/adapters/input/readline.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/notifications.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/application_internal.h"

int cli_dispatch_event(struct cli_application *ctx, const struct cli_event_queue_item *item)
{
	int rc = 0;

	if (item->kind == CLI_EVENT_QUEUE_UI_ITEM_MORPH_EVENT)
		rc = cli_runtime_events_deliver(ctx, &item->event);
	else if (item->kind == CLI_EVENT_QUEUE_UI_ITEM_OWNER_CALL) {
		int updating = cli_feed_update_active(ctx);

		/* Only interactive owner calls need a blocking prompt. Keep
		 * normal event refreshes in Readline's terminal mode so input
		 * cannot be echoed, erased or have CR translated by the TTY. */
		cli_readline_suspend_callback();
		cli_feed_update_end(ctx);
		int result = cli_event_queue_run_owner_call(ctx, item->owner_call);
		if (cli_chat_active(ctx)) {
			int restore_rc = morph_tui_application_restore_terminal(&ctx->base);
			if (restore_rc)
				return restore_rc;
		}

		/* A prompt consumes its cancellation key. Do not apply that
		 * key again to the composer draft restored by its adapter. */
		if (result == -ECANCELED || result == -EINTR)
			cli_cancel_state_reset(ctx);
		if (updating)
			cli_feed_update_begin(ctx);
	} else if (ctx->presentation_mode != CLI_PRESENT_EVENTS_JSON) {
		cli_feed_history_begin(ctx);
		cli_notifications_render(ctx, item);
		cli_feed_history_end(ctx);
	}
	fflush(stdout);
	return rc;
}
