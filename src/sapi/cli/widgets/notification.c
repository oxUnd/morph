#include "sapi/cli/widgets/notification.h"
#include "sapi/cli/widgets/feed_block.h"
#include "util/cleanup.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/event_queue_item.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/adapters/media/renderer.h"
#include "sapi/cli/application.h"

void cli_notification_write(struct cli_application *ctx, const struct cli_event_queue_item *item)
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

	int interactive = ctx->presentation_mode == CLI_PRESENT_INTERACTIVE;
	if (interactive) {
		morph_buf_t title MORPH_CLEANUP(morph_buf_cleanup) = {0};
		(void)morph_buf_puts(&title, "[task] ");
		if (item->notification_task_id > 0)
			(void)morph_buf_printf(&title, "#%lld ", (long long)item->notification_task_id);
		(void)morph_buf_printf(&title, "%s · %s",
			item->notification_title ? item->notification_title : "Task", status);
		(void)cli_feed_notice_write(ctx, item->notification_failed ? CLI_NOTICE_FAILED
			: item->notification_warning ? CLI_NOTICE_WARNING : CLI_NOTICE_DONE,
			morph_buf_cstr_borrow(&title), NULL, NULL);
	} else {
	cli_output_printf(ctx, "\n%s%s" ANSI_RESET " " ANSI_CYAN "[task]" ANSI_RESET " ", color, mark);
	if (item->notification_task_id > 0)
		cli_output_printf(ctx, ANSI_DIM "#%lld " ANSI_RESET, (long long)item->notification_task_id);
	(void)cli_output_print_untrusted(
		ctx, item->notification_title ? item->notification_title : "Task",
		UTF8_TERMINAL_TEXT_SINGLE_LINE);
	cli_output_printf(ctx, ANSI_DIM " · " ANSI_RESET "%s%s" ANSI_RESET "\n", color, status);
	}
	if (item->notification_body && item->notification_body[0]) {
		cli_output_printf(ctx, interactive ? "├ " : "  ");
		cli_markdown_render_ansi_with_media_indented(ctx, item->notification_body, 2,
							     cli_renderer_media, ctx);
	}
	cli_output_printf(ctx, "%s" ANSI_DIM "inbox #%lld", interactive ? "└ " : "  ",
		(long long)item->notification_id);
	if (created > 0 && localtime_r(&created, &local) &&
	    strftime(clock, sizeof(clock), "%H:%M:%S", &local) > 0)
		cli_output_printf(ctx, " · %s", clock);
	cli_output_printf(ctx, "%s", interactive ? ANSI_RESET "\n" : ANSI_RESET "\n\n");
}
