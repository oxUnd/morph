#include "sapi/cli/presentation/notifications.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/event_queue_item.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/adapters/media/renderer.h"
#include "sapi/cli/application_internal.h"

void cli_notifications_render(struct cli_application *ctx, const struct cli_event_queue_item *item)
{
	const char *color = item->notification_failed	 ? CLI_FEED_FAILED_STYLE
			    : item->notification_warning ? ANSI_YELLOW
							 : ANSI_GREEN;
	const char *mark = item->notification_failed	? CLI_FEED_FAILED
			   : item->notification_warning ? CLI_FEED_WARNING
							: CLI_FEED_DONE;
	const char *status = item->notification_failed	  ? "failed"
			     : item->notification_warning ? "attention"
							  : "completed";
	char clock[32] = "";
	struct tm local;
	time_t created = (time_t)item->notification_created_at;

	printf(ctx, "\n%s%s" ANSI_RESET " " ANSI_CYAN "[task]" ANSI_RESET " ", color, mark);
	if (item->notification_task_id > 0)
		printf(ctx, ANSI_DIM "#%lld " ANSI_RESET, (long long)item->notification_task_id);
	(void)cli_output_print_untrusted(
		ctx, item->notification_title ? item->notification_title : "Task",
		UTF8_TERMINAL_TEXT_SINGLE_LINE);
	printf(ctx, ANSI_DIM " · " ANSI_RESET "%s%s" ANSI_RESET "\n", color, status);
	if (item->notification_body && item->notification_body[0]) {
		printf(ctx, "  ");
		cli_markdown_render_ansi_with_media_indented(ctx, item->notification_body, 2,
							     cli_renderer_media, ctx);
	}
	printf(ctx, "  " ANSI_DIM "inbox #%lld", (long long)item->notification_id);
	if (created > 0 && localtime_r(&created, &local) &&
	    strftime(clock, sizeof(clock), "%H:%M:%S", &local) > 0)
		printf(ctx, " · %s", clock);
	printf(ctx, ANSI_RESET "\n\n");
}
