#include "sapi/cli/services/turn.h"
#include "sapi/cli/application.h"
#include "sapi/cli/services/agents.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/services/command_job.h"
#include "agent/turn.h"
#include "sapi/cli/adapters/input/callbacks.h"
#include "sapi/cli/adapters/input/cancel.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/services/scheduler.h"
#include "sapi/cli/services/recap.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/services/event_queue.h"
#include "runtime/runtime.h"
#include <stdarg.h>
#include "credits.h"
#include "sapi/cli/models/composer.h"

void cli_turn_record_media_credits(struct cli_application *ctx, const char *kind,
				   int64_t image_units, int64_t video_seconds, const char *provider,
				   const char *model, const char *metadata_json)
{
	if (!ctx || !kind)
		return;
	(void)runtime_credit_record_media(ctx->runtime, kind, image_units, video_seconds, provider,
					  model, metadata_json);
}
static void cli_turn_record_image(void *data, int width, int height)
{
	struct cli_application *ctx = data;
	const struct config *config = runtime_config_borrow(ctx->runtime);

	if (config)
		cli_turn_record_media_credits(
			ctx, "image_input", credit_image_units_from_size(width, height), 0,
			config->models.image.provider, config->models.image.model, NULL);
}

void cli_turn_record_input_images(struct cli_application *ctx, const char *input)
{
	cli_composer_model_visit_images(&ctx->composer, input, cli_turn_record_image, ctx);
}

void cli_turn_prepare(struct cli_application *ctx)
{
	if (!ctx)
		return;
	(void)runtime_turn_prepare_tools(ctx->runtime, (int64_t)time(NULL));
	cli_scheduler_process_due(ctx);
}

int cli_turn_execute_prompt(struct cli_application *ctx, const char *input)
{
	int owns_turn = 0;

	if (!ctx || !input)
		return -EINVAL;
	morph_buf_t input_buf;
	const char *effective_input = input;
	int has_input_buf = 0;

	memset(&input_buf, 0, sizeof(input_buf));
	if (ctx->image_path[0]) {
		int rc = morph_buf_init(&input_buf, 256);

		if (rc != 0)
			return rc;
		has_input_buf = 1;
		rc = morph_buf_printf(
			&input_buf,
			"[Image: %s]\n"
			"(The user attached the image above; if the task involves "
			"understanding or reading the image, use img_qa to view it.)\n%s",
			ctx->image_path, input);
		if (rc != 0) {
			morph_buf_cleanup(&input_buf);
			return rc;
		}
		effective_input = morph_buf_cstr_borrow(&input_buf);
		ctx->image_path[0] = '\0';
	}

	if (!ctx->turn_active) {
		cli_turn_begin(ctx);
		owns_turn = 1;
	}
	if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE && !ctx->input_job)
		ctx->cancel_monitor = cli_cancel_monitor_start(ctx, STDIN_FILENO);
	struct session current;
	(void)runtime_session_current(ctx->runtime, &current);
	struct runtime_request request = {
		.session_id = current.id,
		.model_input = effective_input,
		.stored_user_input = input,
		.generate_session_title = !ctx->session_auto_named,
		.prompt_pending_fn = cli_command_job_prompt_pending,
		.action_drain_fn = cli_command_job_drain,
		.action_drain_user_data = ctx->input_job,
		.override_action_drain = ctx->input_job != NULL,
		.output_cb = NULL,
		.output_user_data = NULL,
		.turn_flags = AGENT_TURN_DEFAULT_FLAGS | AGENT_TURN_SAVE_EMPTY_USER |
			      AGENT_TURN_SAVE_EMPTY_ASSISTANT,
	};
	struct runtime_result runtime_result;
	int react_rc = runtime_execute_turn(ctx->runtime, &request, &runtime_result);
	if (react_rc != -EBUSY)
		ctx->session_auto_named = 1;
	if (has_input_buf)
		morph_buf_cleanup(&input_buf);
	struct cli_cancel_monitor *cancel_monitor = ctx->cancel_monitor;
	ctx->cancel_monitor = NULL;
	cli_cancel_monitor_stop(cancel_monitor);
	if (owns_turn)
		cli_turn_finish(ctx, react_rc);
	if (react_rc == -EBUSY)
		return react_rc;

	cli_scheduler_process_due(ctx);
	return react_rc;
}

void cli_turn_begin(struct cli_application *ctx)
{
	if (!ctx)
		return;
	(void)cli_agents_refresh(ctx);
	cli_cancel_state_reset(ctx);
	cli_presentation_reset(ctx);
	ctx->turn_active = 1;
	cli_feed_turn_begin(ctx);
	if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE)
		cli_feed_render_frame(ctx, 1);
}

void cli_turn_finish(struct cli_application *ctx, int turn_rc)
{
	if (!ctx)
		return;
	(void)cli_agents_refresh(ctx);
	cli_cancel_state_reset(ctx);
	cli_presentation_finish(ctx);
	ctx->turn_active = 0;
	cli_recap_turn_finished(ctx, cli_recap_now());
	cli_feed_turn_end(ctx, turn_rc);
	if (turn_rc >= 0 || cli_presentation_final_visible(ctx) ||
	    ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON)
		return;
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN)
		cli_output_printf(ctx, "error: %s\n", morph_strerror(turn_rc));
	else
		CMD_ERROR(ctx, "%s", morph_strerror(turn_rc));
}
