#include "sapi/cli/pages/chat.h"
#include "sapi/cli/pages/turn_details.h"
#include "sapi/cli/adapters/input/state.h"
#include "sapi/cli/pages/resume.h"
#include "sapi/cli/services/turn.h"
#include "sapi/cli/application.h"
#include "sapi/cli/services/command_job.h"
#include "sapi/cli/adapters/input/cancel.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/models/composer.h"
#include "sapi/cli/commands/media.h"
#include "sapi/cli/adapters/input/readline.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/services/recap.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/application_internal.h"
#include "util/cleanup.h"

#ifdef HAVE_READLINE
struct cli_chat_page {
	struct cli_application *ctx;
	struct cli_command_job *job;
	int finished_job, job_result;
	int callback_installed;
	char *draft;
	int point;
	morph_buf_t deferred;
};

static void cli_chat_page_schedule(struct cli_chat_page *state, struct morph_tui_application *app)
{
	struct cli_application *ctx = state->ctx;
	int timeout = cli_turn_details_is_open(ctx) ? 250 : cli_feed_next_frame_ms(ctx);

	if (timeout < 0 || timeout > 1000)
		timeout = 1000;
	if (morph_tui_application_depth(app) > 1)
		timeout = 1000;
	else if (!cli_turn_details_is_open(ctx) && cli_command_job_done(state->job))
		timeout = 0;
	morph_tui_application_schedule(app, timeout);
	morph_tui_application_set_input_enabled(app, ctx->running);
	if (!ctx->running && !state->job->active)
		morph_tui_application_stop(app, 0);
}

static int cli_chat_page_appear(void *data, struct morph_tui_application *app)
{
	struct cli_chat_page *state = data;
	struct cli_application *ctx = state->ctx;

	cli_feed_set_hidden(ctx, 0);
	int rc = cli_application_flush(ctx);

	if (rc)
		return rc;
	if (cli_command_job_done(state->job))
		cli_readline_finish_job(ctx);
	ctx->input_status.updated = 0;
	ctx->input_status.suspended = 0;
	if (!ctx->running) {
		cli_chat_page_schedule(state, app);
		return 0;
	}
	if (ctx->resume_requested) {
		/* Open after the root controller's initial transition has committed. */
		cli_chat_page_schedule(state, app);
		morph_tui_application_schedule(app, 0);
		return 0;
	}
	cli_readline_editor_open_take(ctx, &state->draft, state->point);
	state->callback_installed = 1;
	cli_chat_page_schedule(state, app);
	return 0;
}

static void cli_chat_page_disappear(void *data, struct morph_tui_application *app)
{
	struct cli_chat_page *state = data;
	struct cli_application *ctx = state->ctx;

	(void)app;
	if (state->callback_installed) {
		free(state->draft);
		state->draft = cli_readline_suspend_copy(ctx, &state->point);
		cli_readline_editor_close();
		state->callback_installed = 0;
	}
	cli_feed_update_end(ctx);
	cli_feed_set_hidden(ctx, 1);
}

static int cli_chat_page_draw(void *data, const struct morph_tui_screen *screen)
{
	struct cli_chat_page *state = data;

	(void)screen;
	if (state->callback_installed)
		cli_readline_render_frame(state->ctx, 0);
	return 0;
}

static int cli_chat_page_event(void *data, struct morph_tui_application *app,
			       const struct morph_tui_event *event)
{
	struct cli_chat_page *state = data;
	struct cli_application *ctx = state->ctx;
	int modal = morph_tui_application_depth(app) > 1;

	cli_recap_tick(ctx, cli_recap_now());
	if (morph_signal_flag_get(&ctx->terminate_requested)) {
		ctx->running = 0;
		morph_tui_application_stop(app, 0);
		return 1;
	}
	if (ctx->resume_requested && !modal && !morph_tui_application_transition_pending(app)) {
		ctx->resume_requested = 0;
		int rc = cli_resume_show(ctx);

		if (rc)
			CMD_ERROR(ctx, "failed to open sessions: %s", morph_strerror(rc));
		if (!morph_tui_application_transition_pending(app))
			rc = cli_chat_page_appear(state, app);
		return rc ? rc : 1;
	}
	int resized = morph_signal_flag_take(&ctx->resized);
	if (resized || event->type == MORPH_TUI_EVENT_RESIZE) {
		cli_feed_resize(ctx);
		if (modal)
			morph_tui_application_invalidate(app, MORPH_TUI_INVALIDATE_LAYOUT);
		else if (state->callback_installed)
			cli_readline_render_frame(ctx, 1);
	}
	if (morph_signal_flag_take(&ctx->interrupted)) {
		if (modal) {
			int rc = morph_tui_application_dismiss_controller(app, 0);

			if (rc < 0)
				return rc;
		} else {
			if (cli_turn_details_is_open(ctx))
				cli_turn_details_toggle(ctx);
			if (state->callback_installed) {
				cli_readline_editor_replace(ctx, "", 0);
			}
		}
	}
	if (modal && cli_event_queue_owner_call_pending(ctx)) {
		/* An interactive runtime request needs its own input scope. Return
		 * to the composer before invoking the legacy prompt adapter. */
		if (!morph_tui_application_transition_pending(app)) {
			int rc = morph_tui_application_dismiss_controller(app, 0);

			if (rc < 0)
				return rc;
		}
		(void)morph_tui_application_wake(app);
	} else if (event->type == MORPH_TUI_EVENT_WAKE) {
		if (modal) {
			int rc = cli_output_capture_styled_begin(ctx, &state->deferred);

			if (rc)
				return rc;
			ctx->output.capture_history = 1;
			(void)cli_event_queue_drain_batch(ctx);
			cli_output_capture_end(ctx);
			if (state->deferred.failed)
				return state->deferred.failed;
		} else if (state->callback_installed) {
			cli_readline_drain_ui(ctx);
		} else {
			(void)cli_event_queue_drain_batch(ctx);
		}
	}
	if (!modal && !cli_turn_details_is_open(ctx) && cli_command_job_done(state->job)) {
		int point;

		cli_feed_update_begin(ctx);
		char *draft MORPH_CLEANUP(morph_charp_cleanup) =
			cli_readline_suspend_copy(ctx, &point);

		cli_readline_finish_job(ctx);
		if (draft)
			cli_readline_resume_take(ctx, &draft, point);
		cli_feed_update_end(ctx);
	}
	if (!modal && event->type == MORPH_TUI_EVENT_TICK)
		morph_tui_application_invalidate(app, MORPH_TUI_INVALIDATE_DISPLAY);
	cli_chat_page_schedule(state, app);
	return 0;
}

static int cli_chat_page_input(void *data, struct morph_tui_application *app,
			       const struct morph_tui_event *event)
{
	struct cli_chat_page *state = data;
	struct cli_application *ctx = state->ctx;

	if (event->type != MORPH_TUI_EVENT_INPUT_READY)
		return 0;
	int rc = cli_chat_page_event(data, app, event);

	if (rc < 0 || !ctx->running)
		return rc;

	if (!state->job->active)
		cli_cancel_state_reset(ctx);
	cli_readline_editor_read(ctx);
	if (ctx->input->readline_ready_input) {
		char *input = ctx->input->readline_ready_input;
		char *submitted = input;
		char *retry_command = NULL;

		ctx->input->readline_ready_input = NULL;
		cli_readline_clear_footer(ctx);
		cli_readline_editor_close();
		state->callback_installed = 0;
		if (isatty(STDOUT_FILENO))
			fprintf(stdout, "\033[?1004l");
		cli_feed_composer_suspend(ctx);
		if (input[0] != '\0') {
			int handled = 0;
			int command_rc;

			if (input[0] == '/')
				cli_presentation_command_input(ctx, input);
			else if (!state->job->active)
				cli_presentation_user_input(ctx, input);
			cli_readline_editor_remember(input);
			if (input[0] != '/') {
				char *expanded =
					cli_composer_model_expand_copy(&ctx->composer, input);

				if (expanded) {
					cli_turn_record_input_images(ctx, input);
					input = expanded;
				} else {
					handled = 1;
					CMD_ERROR(ctx, "%s", morph_strerror(-ENOMEM));
				}
			}
			if (!handled && state->job->active && strcmp(input, "/recap") == 0) {
				command_rc = cli_recap_start(ctx, 0);
				handled = 1;
			} else if (!handled && state->job->active &&
				   (!strcmp(input, "/jobs") || !strncmp(input, "/jobs ", 6))) {
				command_rc = cli_command_dispatch(ctx, input);
				handled = 1;
			} else if (!handled && state->job->active && input[0] == '/') {
				if (!cli_command_find_input_borrow(input))
					CMD_ERROR(ctx, "unknown command: %s. Try /help", input);
				else {
					CMD_ERROR(ctx, "wait for the active turn "
						       "before running this command");
					retry_command = input;
				}
				handled = 1;
			} else if (!handled && state->job->active) {
				command_rc = cli_command_job_prompt(state->job, input, submitted);
				if (command_rc != 0)
					CMD_ERROR(ctx, "%s", morph_strerror(command_rc));
				else
					cli_feed_queue_changed(ctx);
				handled = 1;
			} else if (!handled) {
				command_rc = cli_media_handle_path(ctx, input, &handled);
			}
			if (handled || input[0] == '/') {
				if (!handled) {
					command_rc = cli_turn_execute(ctx, input);
				}
				(void)cli_event_queue_drain(ctx);
				if (input[0] == '/' && ctx->running) {
					if (morph_tui_application_transition_pending(app))
						ctx->application_command = 1;
					else
						cli_presentation_command_end(ctx);
				}
			} else {
				cli_turn_begin(ctx);
				command_rc = cli_command_job_start(state->job, ctx, input);
				if (command_rc != 0)
					cli_turn_finish(ctx, command_rc);
			}
		}
		if (submitted != input)
			free(submitted);
		if (input != retry_command)
			free(input);
		if (ctx->running && !morph_tui_application_transition_pending(app)) {
			ctx->input_status.updated = 0;
			ctx->input_status.suspended = 0;
			cli_readline_editor_open_take(ctx, NULL, 0);
			state->callback_installed = 1;
			if (retry_command)
				cli_readline_editor_replace(ctx, retry_command, -1);
		}
		free(retry_command);
	}
	cli_chat_page_schedule(state, app);
	return 1;
}

int cli_chat_create(struct cli_application *ctx, struct cli_command_job *job,
		    struct cli_chat_page **out)
{
	struct cli_chat_page *page = calloc(1, sizeof(*page));

	if (!page)
		return -ENOMEM;
	page->ctx = ctx;
	page->job = job;
	int rc = morph_buf_init(&page->deferred, BUFSIZ);

	if (rc) {
		free(page);
		return rc;
	}
	ctx->chat = page;
	ctx->application_output = &page->deferred;
	*out = page;
	return 0;
}

struct morph_tui_application_options cli_chat_options(struct cli_chat_page *page)
{
	return (struct morph_tui_application_options){
		.external_signals = 1,
		.input = stdin,
		.output = stdout,
		.event = cli_chat_page_event,
		.data = page,
	};
}

struct morph_tui_controller cli_chat_controller(struct cli_chat_page *page)
{
	static const struct morph_tui_controller_ops ops = {
		.event = cli_chat_page_input,
		.draw = cli_chat_page_draw,
		.appear = cli_chat_page_appear,
		.disappear = cli_chat_page_disappear,
	};
	return (struct morph_tui_controller){
		.ops = &ops,
		.data = page,
		.input_mode = MORPH_TUI_INPUT_EXTERNAL,
	};
}

void cli_chat_job_finished(struct cli_chat_page *page, int result)
{
	if (page) {
		page->finished_job = 1;
		page->job_result = result;
	}
}

void cli_chat_destroy(struct cli_chat_page *page, int app_rc)
{
	struct cli_application *ctx = page->ctx;

	free(page->draft);
	morph_buf_cleanup(&page->deferred);
	if (app_rc < 0) {
		runtime_cancel_turn(ctx->runtime);
		CMD_ERROR(ctx, "Application failed: %s", morph_strerror(app_rc));
	}
	if (cli_turn_details_is_open(ctx))
		cli_turn_details_toggle(ctx);
	if (isatty(STDOUT_FILENO))
		fprintf(stdout, "\033[?1004l");
	if (ctx->input->readline_ready_input) {
		free(ctx->input->readline_ready_input);
		ctx->input->readline_ready_input = NULL;
	}
	cli_readline_clear_footer(ctx);
	free(ctx->input->footer_text);
	ctx->input->footer_text = NULL;
	if (page->callback_installed)
		cli_readline_editor_close();
	if (page->finished_job) {
		(void)cli_event_queue_drain(ctx);
		cli_turn_finish(ctx, page->job_result);
	}
	ctx->chat = NULL;
	free(page);
}
#endif
