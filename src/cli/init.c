#include "cli/internal.h"
#include "cli/commands/registry.h"

static void cli_img_annotate_pause(void *user_data)
{
	(void)user_data;
	fflush(stdout);
}

static void cli_img_annotate_resume(void *user_data)
{
	(void)user_data;
	fflush(stdout);
}

static void cli_usage_observer(const struct model_usage *usage,
			       void *user_data)
{
	struct cli_context *ctx = user_data;
	struct credit_summary today;
	const struct config *config;

	(void)usage;
	if (!ctx || !ctx->runtime || ctx->event_mode == CLI_EVENTS_JSON)
		return;
	config = runtime_config_get(ctx->runtime);
	if (!config || config->credits.daily_limit < 0)
		return;
	if (runtime_credit_summary_today_get(ctx->runtime, &today) == 0 &&
	    today.credits > config->credits.daily_limit) {
		printf(ANSI_YELLOW "credits warning: %lld / %d today"
		       ANSI_RESET "\n", (long long)today.credits,
		       config->credits.daily_limit);
	}
}

int cli_init(struct cli_context *ctx, const char *config_path,
	     const char *workdir, enum cli_event_mode event_mode)
{
	struct runtime_options options;
	struct scheduled_task_event_sink task_events;
	int rc;

	if (!ctx)
		return -EINVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->event_mode = event_mode;
	ctx->event_cb = cli_event_callback;
	ctx->event_user_data = ctx;
	(void)cli_commands_init();
	cli_emit_startup_event(ctx, "startup.begin", "begin",
			       "startup started", "cli", 0);

	memset(&options, 0, sizeof(options));
	task_events = cli_task_event_sink(ctx);
	options.config_path = config_path ? config_path : default_config_path;
	options.db_path = default_db_path;
	options.workdir = workdir;
	options.front_name = "cli";
	options.event_cb = ctx->event_cb;
	options.event_user_data = ctx;
	options.usage_observer = cli_usage_observer;
	options.usage_observer_user_data = ctx;
	options.hitl_cb = hitl_approval_callback;
	options.hitl_user_data = ctx;
	options.ask_user_cb = cli_ask_user_callback;
	options.ask_user_user_data = ctx;
	options.operation_approval_cb = operation_approval_callback;
	options.operation_approval_user_data = ctx;
	options.background_cb = cli_turn_background_cb;
	options.background_user_data = ctx;
	options.task_events = &task_events;
	options.img_annotate_pause_cb = cli_img_annotate_pause;
	options.img_annotate_resume_cb = cli_img_annotate_resume;
	options.img_annotate_user_data = ctx;
	options.enable_bash = 1;
	options.enable_config_write = 1;
	options.enable_img_annotate = 1;
	options.enable_shell_exts = 1;
	options.enable_sub_agents = 1;
	options.allocate_skill_registry = 1;
	options.auto_connect_mcp = 1;
	rc = runtime_open(&options, &ctx->runtime);
	if (rc != 0) {
		cli_emit_startup_event(ctx, "startup.failed", "failed",
				       "startup failed", "cli", rc);
		return rc;
	}

	ctx->session_auto_named = 0;
	ctx->running = 1;
	ctx->streaming = 0;
	ctx->image_path[0] = '\0';
	rc = cli_sync_start(ctx);
	if (rc != 0)
		log_warn("failed to start sync worker: %s", morph_strerror(rc));
	cli_emit_startup_event(ctx, "startup.ready", "ready",
			       "startup ready", "cli", 0);
	return 0;
}
