#include "sapi/cli/internal.h"
#include "sapi/cli/commands/registry.h"

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
	if (!ctx || !ctx->runtime ||
	    ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON)
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
	     const char *workdir, enum cli_presentation_mode mode)
{
	struct runtime_options options;
	int rc;

	if (!ctx)
		return -EINVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->presentation_mode = mode;
	ctx->event_cb = cli_event_callback;
	ctx->event_user_data = ctx;
	rc = cli_presentation_init(ctx);
	if (rc != 0)
		return rc;
	(void)cli_commands_init();
	memset(&options, 0, sizeof(options));
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
		cli_presentation_cleanup(ctx);
		return rc;
	}

	ctx->session_auto_named = 0;
	ctx->running = 1;
	ctx->presentation_ready = 1;
	ctx->image_path[0] = '\0';
	rc = cli_sync_start(ctx);
	if (rc != 0)
		log_warn("failed to start sync worker: %s", morph_strerror(rc));
	return 0;
}
