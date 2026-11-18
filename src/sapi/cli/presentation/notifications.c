#include "sapi/cli/presentation/notifications.h"
#include "sapi/cli/adapters/media/renderer.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/feed_block.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/presentation/notice.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/event_queue_item.h"
#include <nb/cleanup.h>
#include <time.h>

void cli_notifications_render(struct cli_application *ctx, const struct cli_event_queue_item *item)
{
	enum cli_notice_status outcome = item->notification_failed    ? CLI_NOTICE_FAILED
					 : item->notification_warning ? CLI_NOTICE_WARNING
								      : CLI_NOTICE_DONE;
	const char *status = outcome == CLI_NOTICE_FAILED    ? "failed"
			     : outcome == CLI_NOTICE_WARNING ? "attention"
							     : "completed";
	const char *title = item->notification_title ? item->notification_title : "Task";
	int interactive = ctx->presentation_mode == CLI_PRESENT_INTERACTIVE;

	if (interactive) {
		nb_buf heading NB_CLEANUP(nb_buf_cleanup) = {0};
		(void)nb_buf_append_cstr(&heading, "[task] ");
		if (item->notification_task_id > 0)
			(void)nb_buf_printf(&heading, "#%lld ",
					    (long long)item->notification_task_id);

		(void)nb_buf_printf(&heading, "%s · %s", title, status);
		(void)cli_feed_notice_write(ctx, outcome, nb_buf_cstr(&heading), NULL, NULL);
	} else {
		const char *color = cli_notice_color(outcome);
		cli_output_printf(ctx, "\n%s%s" ANSI_RESET " " ANSI_CYAN "[task]" ANSI_RESET " ",
				  color, cli_notice_marker(outcome));
		if (item->notification_task_id > 0)
			cli_output_printf(ctx, ANSI_DIM "#%lld " ANSI_RESET,
					  (long long)item->notification_task_id);

		(void)cli_output_print_untrusted(ctx, title, MORPH_TERMINAL_TEXT_SINGLE_LINE);
		cli_output_printf(ctx, ANSI_DIM " · " ANSI_RESET "%s%s" ANSI_RESET "\n", color,
				  status);
	}

	if (item->notification_body && item->notification_body[0]) {
		cli_output_printf(ctx, interactive ? "├ " : "  ");
		cli_markdown_render_ansi_with_media_indented(ctx, item->notification_body, 2,
							     cli_renderer_media, ctx);
	}

	char clock[32];
	struct tm local;
	time_t created = (time_t)item->notification_created_at;
	cli_output_printf(ctx, "%s" ANSI_DIM "inbox #%lld", interactive ? "└ " : "  ",
			  (long long)item->notification_id);
	if (created > 0 && localtime_r(&created, &local) &&
	    strftime(clock, sizeof(clock), "%H:%M:%S", &local) > 0)
		cli_output_printf(ctx, " · %s", clock);

	cli_output_printf(ctx, "%s", interactive ? ANSI_RESET "\n" : ANSI_RESET "\n\n");
}
