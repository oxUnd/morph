#include "sapi/cli/adapters/runtime/observer.h"
#include "sapi/cli/application_internal.h"
#include "credits.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/widgets/feed.h"

void cli_runtime_observer_pause(void *user_data)
{
	(void)user_data;
	fflush(stdout);
}

void cli_runtime_observer_resume(void *user_data)
{
	(void)user_data;
	fflush(stdout);
}

struct cli_runtime_observer_credit_warning {
	struct cli_application *ctx;
	int64_t credits;
	int daily_limit;
};

static int cli_runtime_observer_credit_warning_on_owner(void *opaque)
{
	struct cli_runtime_observer_credit_warning *warning = opaque;
	struct cli_application *ctx = warning->ctx;
	(void)ctx;

	cli_feed_history_begin(warning->ctx);
	printf(ctx, ANSI_YELLOW "credits warning: %lld / %d today" ANSI_RESET "\n",
	       (long long)warning->credits, warning->daily_limit);
	cli_feed_history_end(warning->ctx);
	return 0;
}

void cli_runtime_observer_usage(const struct model_usage *usage, void *user_data)
{
	struct cli_application *ctx = user_data;
	struct credit_summary today;
	const struct config *config;

	(void)usage;
	if (!ctx || !ctx->runtime || ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON)
		return;
	config = runtime_config_borrow(ctx->runtime);
	if (!config || config->credits.daily_limit < 0)
		return;
	if (runtime_credit_summary_today_get(ctx->runtime, &today) == 0 &&
	    today.credits > config->credits.daily_limit) {
		struct cli_runtime_observer_credit_warning warning = {
			ctx,
			today.credits,
			config->credits.daily_limit,
		};

		if (ctx->events)
			(void)cli_event_queue_call_owner(
				ctx, cli_runtime_observer_credit_warning_on_owner, &warning);
		else
			(void)cli_runtime_observer_credit_warning_on_owner(&warning);
	}
}
