#include "sapi/cli/ui/application.h"
#ifdef HAVE_READLINE
#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>
#endif
#include "sapi/cli/command_job.h"
#include "sapi/cli/ui/input/callbacks.h"
#include "sapi/cli/ui/input/cancel.h"
#include "sapi/cli/core.h"
#include "sapi/cli/ui/input/composer.h"
#include "sapi/cli/commands/media.h"
#include "sapi/cli/ui/input/readline.h"
#include "sapi/cli/ui/presentation/presenter.h"
#include "sapi/cli/ui/recap.h"
#include "sapi/cli/ui/presentation/terminal.h"
#include "sapi/cli/ui/presentation/transcript.h"
#include "sapi/cli/ui/event_queue.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/internal.h"

struct cli_application_screen {
	struct cli_context *ctx;
	struct morph_tui_screen_ops ops;
	void *data;
	void (*complete)(void *data, int result);
};

static int cli_application_screen_event(void *data, struct morph_tui_application *app,
					const struct morph_tui_event *event)
{
	struct cli_application_screen *screen = data;
	int rc = screen->ops.update(screen->data, event);

	if (rc == MORPH_TUI_SCREEN_CLOSE)
		return morph_tui_application_dismiss_controller(app, 0);
	if (rc == MORPH_TUI_SCREEN_REDRAW)
		morph_tui_application_invalidate(app, MORPH_TUI_INVALIDATE_DISPLAY);
	return rc < 0 ? rc : 1;
}

static int cli_application_screen_draw(void *data, const struct morph_tui_screen *terminal)
{
	struct cli_application_screen *screen = data;

	return screen->ops.draw(screen->data, terminal);
}

int cli_application_flush(struct cli_context *ctx)
{
	morph_buf_t *pending = ctx->application_output;

	if (pending && pending->failed)
		return pending->failed;
	if (pending && pending->len) {
		if (fwrite(morph_buf_cstr(pending), 1, pending->len, stdout) != pending->len ||
		    fflush(stdout) != 0)
			return errno ? -errno : -EIO;
		morph_buf_reset(pending);
	}
	return 0;
}

void cli_application_panel_complete(struct cli_context *ctx, void *data,
				    void (*complete)(void *, int), int result)
{
	if (morph_tui_application_depth(ctx->application) <= 1) {
		int rc = cli_application_flush(ctx);

		if (rc) {
			result = rc;
			morph_tui_application_stop(ctx->application, rc);
		}
	}
	complete(data, result);
	if (ctx->application_command && morph_tui_application_depth(ctx->application) <= 1) {
		ctx->application_command = 0;
		if (result < 0 && result != -ECANCELED)
			CMD_ERROR(ctx, "%s", morph_strerror(result));
		cli_presentation_command_end(ctx);
	}
}

static void cli_application_screen_close(void *data, int result)
{
	struct cli_application_screen *screen = data;

	cli_application_panel_complete(screen->ctx, screen->data, screen->complete, result);
	free(screen);
}

int cli_application_present_screen(struct cli_context *ctx, const struct morph_tui_screen_ops *ops,
				   void *data, void (*complete)(void *data, int result))
{
	if (!ctx || !ctx->application || !ops || !ops->update || !ops->draw || !complete)
		return -EINVAL;
	struct cli_application_screen *screen = calloc(1, sizeof(*screen));

	if (!screen)
		return -ENOMEM;
	*screen = (struct cli_application_screen){
		.ctx = ctx, .ops = *ops, .data = data, .complete = complete};
	static const struct morph_tui_controller_ops controller_ops = {
		.event = cli_application_screen_event,
		.draw = cli_application_screen_draw,
		.close = cli_application_screen_close,
	};
	struct morph_tui_controller controller = {
		.ops = &controller_ops,
		.data = screen,
		.input_mode = MORPH_TUI_INPUT_KEYS,
		.terminal_flags =
			MORPH_TUI_TERMINAL_SESSION_ALTERNATE | MORPH_TUI_TERMINAL_SESSION_PASTE,
		.tick_ms = 100,
	};
	int rc = morph_tui_application_present_controller(ctx->application, &controller);

	if (rc)
		free(screen);
	return rc;
}

#ifdef HAVE_READLINE
struct cli_application_root {
	struct morph_tui_application base;
	struct cli_context *ctx;
	struct cli_command_job *job;
	int finished_job, job_result;
	int callback_installed;
	char *draft;
	int point;
	morph_buf_t deferred;
};

static void cli_application_root_prepare_termination(struct morph_tui_application *app, int result)
{
	struct cli_application_root *state = (struct cli_application_root *)app;
	struct cli_context *ctx = state->ctx;

	(void)result;
	ctx->running = 0;
	/* A worker may be waiting for an owner call. Release it before joining;
	 * preserve queued output for presentation after terminal restoration. */
	(void)cli_event_queue_stop_owner_calls(ctx);
	if (state->job->active) {
		runtime_cancel_turn(ctx->runtime);
		state->job_result = cli_command_job_finish(state->job);
		state->finished_job = 1;
	}
	cli_event_queue_attach_app(ctx, NULL);
}

static void cli_application_root_schedule(struct cli_application_root *state,
					  struct morph_tui_application *app)
{
	struct cli_context *ctx = state->ctx;
	int timeout = ctx->details_open ? 250 : cli_terminal_next_frame_ms(ctx);

	if (timeout < 0 || timeout > 1000)
		timeout = 1000;
	if (morph_tui_application_depth(app) > 1)
		timeout = 1000;
	else if (!ctx->details_open && cli_command_job_done(state->job))
		timeout = 0;
	morph_tui_application_schedule(app, timeout);
	morph_tui_application_set_input_enabled(app, ctx->running);
	if (!ctx->running && !state->job->active)
		morph_tui_application_stop(app, 0);
}

static int cli_application_root_appear(void *data, struct morph_tui_application *app)
{
	struct cli_application_root *state = data;
	struct cli_context *ctx = state->ctx;

	cli_terminal_set_hidden(ctx, 0);
	int rc = cli_application_flush(ctx);

	if (rc)
		return rc;
	if (cli_command_job_done(state->job))
		cli_readline_finish_job(ctx);
	ctx->input_status.updated = 0;
	ctx->input_status.suspended = 0;
	if (!ctx->running) {
		cli_application_root_schedule(state, app);
		return 0;
	}
	cli_terminal_composer_resume(ctx);
	rl_callback_handler_install(state->draft ? "" : cli_readline_input_prompt(ctx),
				    cli_callback_line(ctx, cli_readline_line_ready));
	state->callback_installed = 1;
	cli_readline_configure(ctx);
	if (state->draft) {
		char *draft = state->draft;

		state->draft = NULL;
		rl_set_prompt(cli_readline_input_prompt(ctx));
		cli_readline_resume(ctx, draft, state->point);
	}
	cli_application_root_schedule(state, app);
	return 0;
}

static void cli_application_root_disappear(void *data, struct morph_tui_application *app)
{
	struct cli_application_root *state = data;
	struct cli_context *ctx = state->ctx;

	(void)app;
	if (state->callback_installed) {
		free(state->draft);
		state->draft = cli_readline_suspend(ctx, &state->point);
		rl_callback_handler_remove();
		state->callback_installed = 0;
	}
	cli_terminal_update_end(ctx);
	cli_terminal_set_hidden(ctx, 1);
}

static int cli_application_root_draw(void *data, const struct morph_tui_screen *screen)
{
	struct cli_application_root *state = data;

	(void)screen;
	if (state->callback_installed)
		cli_readline_render_frame(state->ctx, 0);
	return 0;
}

static int cli_application_root_event(void *data, struct morph_tui_application *app,
				      const struct morph_tui_event *event)
{
	struct cli_application_root *state = data;
	struct cli_context *ctx = state->ctx;
	int modal = morph_tui_application_depth(app) > 1;

	cli_recap_tick(ctx, cli_recap_now());
	if (morph_signal_flag_get(&ctx->terminate_requested)) {
		ctx->running = 0;
		morph_tui_application_stop(app, 0);
		return 1;
	}
	if (morph_signal_flag_take(&ctx->resized)) {
		cli_terminal_resize(ctx);
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
			if (ctx->details_open)
				cli_transcript_toggle(ctx);
			if (state->callback_installed) {
				rl_replace_line("", 0);
				rl_point = 0;
				cli_readline_redisplay(ctx);
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
			int rc = cli_command_capture_styled_begin(ctx, &state->deferred);

			if (rc)
				return rc;
			ctx->output.capture_history = 1;
			(void)cli_event_queue_drain_batch(ctx);
			cli_command_capture_end(ctx);
			if (state->deferred.failed)
				return state->deferred.failed;
		} else if (state->callback_installed) {
			cli_readline_drain_ui(ctx);
		} else {
			(void)cli_event_queue_drain_batch(ctx);
		}
	}
	if (!modal && !ctx->details_open && cli_command_job_done(state->job)) {
		int point;

		cli_terminal_update_begin(ctx);
		char *draft = cli_readline_suspend(ctx, &point);

		cli_readline_finish_job(ctx);
		if (draft)
			cli_readline_resume(ctx, draft, point);
		cli_terminal_update_end(ctx);
	}
	if (!modal && event->type == MORPH_TUI_EVENT_TICK)
		morph_tui_application_invalidate(app, MORPH_TUI_INVALIDATE_DISPLAY);
	cli_application_root_schedule(state, app);
	return 0;
}

static int cli_application_root_input(void *data, struct morph_tui_application *app,
				      const struct morph_tui_event *event)
{
	struct cli_application_root *state = data;
	struct cli_context *ctx = state->ctx;

	if (event->type != MORPH_TUI_EVENT_INPUT_READY)
		return 0;
	int rc = cli_application_root_event(data, app, event);

	if (rc < 0 || !ctx->running)
		return rc;

	int previous_point = rl_point;
	int image_start;
	int image_end;

	if (ctx->details_open) {
		cli_readline_details_input(ctx);
		return 1;
	}
	if (!state->job->active)
		cli_cancel_state_reset(ctx);
	cli_readline_clear_footer(ctx);
	rl_callback_read_char();
	if (!ctx->readline_ready_input &&
	    cli_composer_image_span(&ctx->composer, rl_line_buffer, rl_point, 0, &image_start,
				    &image_end) &&
	    rl_point > image_start) {
		rl_point = rl_point < previous_point ? image_start : image_end;
		cli_readline_redisplay(ctx);
	}
	if (ctx->readline_ready_input) {
		char *input = ctx->readline_ready_input;
		char *submitted = input;
		char *retry_command = NULL;

		ctx->readline_ready_input = NULL;
		cli_readline_clear_footer(ctx);
		rl_callback_handler_remove();
		state->callback_installed = 0;
		if (isatty(STDOUT_FILENO))
			fprintf(stdout, "\033[?1004l");
		cli_terminal_composer_suspend(ctx);
		if (input[0] != '\0') {
			int handled = 0;
			int command_rc;

			if (input[0] == '/')
				cli_presentation_command_input(ctx, input);
			else if (!state->job->active)
				cli_presentation_user_input(ctx, input);
			add_history(input);
			if (input[0] != '/') {
				char *expanded = cli_composer_expand(&ctx->composer, input);

				if (expanded) {
					cli_composer_record_images(&ctx->composer, ctx, input);
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
				if (!cli_command_find_input(input))
					CMD_ERROR(ctx, "unknown command: %s. Try /help", input);
				else
					CMD_ERROR(ctx, "wait for the active turn "
						       "before running this command");
				retry_command = input;
				handled = 1;
			} else if (!handled && state->job->active) {
				command_rc = cli_command_job_prompt(state->job, input, submitted);
				if (command_rc != 0)
					CMD_ERROR(ctx, "%s", morph_strerror(command_rc));
				else
					cli_terminal_queue_changed(ctx);
				handled = 1;
			} else if (!handled) {
				command_rc = cli_handle_media_path(ctx, input, &handled);
			}
			if (handled || input[0] == '/') {
				if (!handled) {
					if (!cli_command_find_input(input))
						retry_command = input;
					command_rc = cli_handle_command(ctx, input);
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
			cli_terminal_composer_resume(ctx);
			rl_callback_handler_install(
				cli_readline_input_prompt(ctx),
				cli_callback_line(ctx, cli_readline_line_ready));
			cli_readline_configure(ctx);
			state->callback_installed = 1;
			if (retry_command) {
				rl_replace_line(retry_command, 0);
				rl_point = rl_end;
				cli_readline_redisplay(ctx);
			}
		}
		free(retry_command);
	}
	cli_application_root_schedule(state, app);
	return 1;
}

static void cli_application_run_session(struct cli_context *ctx, struct cli_command_job *job)
{
	struct cli_application_root state = {.ctx = ctx, .job = job};
	static const struct morph_tui_application_ops lifecycle_ops = {
		.prepare_termination = cli_application_root_prepare_termination,
	};

	state.base.ops = &lifecycle_ops;
	const struct morph_tui_application_options app_options = {
		.external_signals = 1,
		.input = stdin,
		.output = stdout,
		.event = cli_application_root_event,
		.data = &state,
	};
	const struct morph_tui_controller_ops app_ops = {
		.event = cli_application_root_input,
		.draw = cli_application_root_draw,
		.appear = cli_application_root_appear,
		.disappear = cli_application_root_disappear,
	};
	const struct morph_tui_controller root = {
		.ops = &app_ops,
		.data = &state,
		.input_mode = MORPH_TUI_INPUT_EXTERNAL,
	};
	int app_rc = morph_buf_init(&state.deferred, BUFSIZ);

	if (!app_rc)
		app_rc = morph_tui_application_init(&state.base, &app_options);
	if (!app_rc) {
		ctx->application = &state.base;
		ctx->application_output = &state.deferred;
		cli_event_queue_attach_app(ctx, ctx->application);
		app_rc = morph_tui_application_present_controller(ctx->application, &root);
		if (!app_rc)
			app_rc = morph_tui_application_run(ctx->application);
		morph_tui_application_deinit(ctx->application);
		cli_terminal_set_hidden(ctx, 0);
		int flush_rc = cli_application_flush(ctx);

		if (!app_rc)
			app_rc = flush_rc;
		ctx->application = NULL;
		ctx->application_output = NULL;
	}
	free(state.draft);
	morph_buf_cleanup(&state.deferred);
	if (app_rc < 0) {
		runtime_cancel_turn(ctx->runtime);
		CMD_ERROR(ctx, "UI application failed: %s", morph_strerror(app_rc));
	}
	if (ctx->details_open)
		cli_transcript_toggle(ctx);
	if (isatty(STDOUT_FILENO))
		fprintf(stdout, "\033[?1004l");
	if (ctx->readline_ready_input) {
		free(ctx->readline_ready_input);
		ctx->readline_ready_input = NULL;
	}
	cli_readline_clear_footer(ctx);
	free(ctx->footer_text);
	ctx->footer_text = NULL;
	if (state.callback_installed)
		rl_callback_handler_remove();
	if (state.finished_job) {
		(void)cli_event_queue_drain(ctx);
		cli_turn_finish(ctx, state.job_result);
	}
}

void cli_application_run(struct cli_context *ctx)
{
	struct cli_command_job job;
	int rc = cli_command_job_init(&job);

	if (rc) {
		CMD_ERROR(ctx, "failed to initialize command worker: %s", morph_strerror(rc));
		return;
	}
	ctx->input_job = &job;
	cli_readline_run(ctx, &job, cli_application_run_session);
	ctx->input_job = NULL;
	cli_command_job_cleanup(&job);
}
#endif
