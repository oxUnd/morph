#include "sapi/cli/application.h"
#include <nb/err.h>
#include "runtime/runtime.h"
#include <nb/cleanup.h>
#include <nb/time.h>
#include "sapi/cli/adapters/input/state.h"
#include "sapi/cli/adapters/runtime/dispatch.h"
#include "sapi/cli/services/turn.h"
#include "sapi/cli/services/agents.h"
#include "sapi/cli/models/jobs.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/pages/resume.h"
#include "sapi/cli/services/session_restore.h"
#include "sapi/cli/services/sessions.h"
#include "sapi/cli/adapters/runtime/observer.h"
#include <stdio.h>
#include "sapi/cli/services/command_job.h"
#include "sapi/cli/adapters/input/callbacks.h"
#include "sapi/cli/adapters/input/cancel.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/models/composer.h"
#include "sapi/cli/commands/media.h"
#include "sapi/cli/commands/input.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/services/recap.h"
#include "sapi/cli/presentation/feed.h"
#include "sapi/cli/presentation/transcript.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/application_internal.h"
#include "credits.h"
#include "config/config.h"
#include "sapi/cli/pages/question.h"
#include "sapi/cli/adapters/runtime/events.h"
#include "sapi/cli/services/scheduler.h"
#include "sapi/cli/pages/approval.h"
#include "sapi/cli/adapters/input/reader.h"
#include "sapi/cli/presentation/banner.h"
#include "sapi/cli/pages/hooks.h"
#include "sapi/cli/pages/subagents.h"
#include "sapi/cli/adapters/protocol/interaction.h"
#include "sapi/cli/adapters/clipboard/clipboard.h"
#include <nothing/tui/window.h>
#include "cJSON.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#define CLI_APPLICATION_WHEEL_SCROLL_CELLS 3
#else
#define CLI_APPLICATION_WHEEL_SCROLL_CELLS 1
#endif

struct cli_application *cli_application_from_controller(const struct nt_view_controller *controller)
{
	return (struct cli_application *)nt_view_controller_application(controller);
}

struct cli_application *cli_application_from_view(const struct nt_view *view)
{
	return cli_application_from_controller(nt_view_controller(view));
}

void cli_application_controller_cleanup(struct nt_view_controller **owner)
{
	nt_view_controller_destroy(*owner);
	*owner = NULL;
}

int cli_application_show_page(struct cli_application *ctx, struct nt_view_controller **owner,
			      int tick_ms)
{
	if (!ctx || !owner || !*owner)
		return NB_ERR_INVAL;

	int rc = nt_view_controller_set_tick_interval(*owner, tick_ms);
	if (rc)
		return rc;

	int standalone = !cli_application_tui_borrow(ctx);
	if (standalone) {
		const struct nt_application_options options = {
			.input = stdin,
			.output = stdout,
			.external_signals = 1,
			.terminal_flags = NT_TERMINAL_MOUSE | NT_TERMINAL_PASTE,
			.synchronized_output = 1,
			.wheel_scroll_columns = CLI_APPLICATION_WHEEL_SCROLL_CELLS,
			.wheel_scroll_lines = CLI_APPLICATION_WHEEL_SCROLL_CELLS};
		rc = nt_application_init(&ctx->base, &options);
		if (rc)
			return rc;

		rc = nt_application_set_color_profile(
			&ctx->base, cli_output_color_enabled(ctx) ? NT_COLOR_PROFILE_TRUECOLOR
								  : NT_COLOR_PROFILE_NONE);
	}

	if (!rc) {
		rc = nt_application_present_view_controller(&ctx->base, owner);
		if (!rc)
			ctx->modal_count++;
	}

	if (!rc && standalone)
		rc = nt_application_run(&ctx->base);

	if (standalone)
		nt_application_deinit(&ctx->base);

	return rc;
}

int cli_application_run_page(struct cli_application *ctx, struct nt_view_controller **owner,
			     FILE *input, FILE *output)
{
	if (!ctx || !owner || !*owner || !input || !output)
		return NB_ERR_INVAL;

	if (cli_application_tui_borrow(ctx))
		return NB_ERR_BUSY;

	const struct nt_application_options options = {
		.input = input,
		.output = output,
		.terminal_flags = NT_TERMINAL_MOUSE | NT_TERMINAL_PASTE | NT_TERMINAL_FOCUS,
		.synchronized_output = 1,
		.wheel_scroll_columns = CLI_APPLICATION_WHEEL_SCROLL_CELLS,
		.wheel_scroll_lines = CLI_APPLICATION_WHEEL_SCROLL_CELLS};
	int rc = nt_application_init(&ctx->base, &options);
	if (rc)
		return rc;

	rc = nt_application_set_color_profile(&ctx->base, cli_output_color_enabled(ctx)
								  ? NT_COLOR_PROFILE_TRUECOLOR
								  : NT_COLOR_PROFILE_NONE);
	if (!rc) {
		rc = nt_application_present_view_controller(&ctx->base, owner);
		if (!rc)
			ctx->modal_count++;
	}

	if (!rc)
		rc = nt_application_run(&ctx->base);

	nt_application_deinit(&ctx->base);
	return rc;
}

int cli_application_prompt_deferred(const struct cli_application *ctx)
{
	return cli_chat_active(ctx) &&
	       (cli_chat_panel_active(ctx) || nt_application_depth(&ctx->base) > 1 ||
		nt_application_transition_pending(&ctx->base));
}

void cli_application_panel_finished(struct cli_application *ctx, int result)
{
	if (!ctx || !ctx->application_command || ctx->modal_count || cli_chat_panel_active(ctx))
		return;

	ctx->application_command = 0;
	if (result < 0 && result != NB_ERR_CANCELLED)
		CMD_ERROR(ctx, "%s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, result));

	cli_presentation_command_end(ctx);
}

static void cli_application_panel_deinit(struct nt_view_controller *controller)
{
	struct cli_application *ctx = cli_application_from_controller(controller);
	if (!ctx)
		return;

	/* A child may already have presented its successor from its deinit. */
	ctx->modal_count--;
	cli_application_panel_finished(ctx, nt_view_controller_result(controller));
}

const struct nt_view_controller_class cli_application_panel_class = {
	.superclass = &nt_view_controller_base_class,
	.instance_size = sizeof(struct nt_view_controller),
	.deinit = cli_application_panel_deinit,
};

static void cli_application_prepare_termination(struct nt_application *base, int result)
{
	struct cli_application *ctx = (struct cli_application *)base;

	(void)result;
	ctx->running = 0;
	if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE)
		(void)cli_event_queue_set_events_enabled(ctx, 0);

	(void)cli_chat_panel_close(ctx, NB_ERR_CANCELLED);
	(void)cli_event_queue_stop_owner_calls(ctx);
	cli_sessions_stop(ctx);
	if (ctx->input_job && ctx->input_job->active) {
		runtime_cancel_turn(ctx->runtime);
		int job_result = cli_command_job_finish(ctx->input_job);
		if (ctx->turn_active)
			cli_turn_finish(ctx, job_result);
	}

	cli_event_queue_attach_app(ctx, NULL);
}

static int cli_application_copy_selection(void *data, const char *text)
{
	struct cli_application *ctx = data;
	int rc = cli_clipboard_copy_text(ctx, text);

	if (rc < 0)
		CMD_ERROR(ctx, "Copy failed: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	else if (rc == CLI_CLIPBOARD_REQUESTED)
		cli_output_printf(ctx, "Copy requested from terminal\n");

	return 0;
}

static void cli_application_run_session(struct cli_application *ctx)
{
	struct nt_application_options options = cli_chat_options();
	options.synchronized_output = 1;
	options.wheel_scroll_columns = CLI_APPLICATION_WHEEL_SCROLL_CELLS;
	options.wheel_scroll_lines = CLI_APPLICATION_WHEEL_SCROLL_CELLS;
	static const struct nt_application_class type = {
		.superclass = &nt_application_base_class,
		.instance_size = sizeof(*ctx),
		.prepare_termination = cli_application_prepare_termination,
	};

	int app_rc = nt_application_init_with_class(&ctx->base, &type, &options);
	if (!app_rc) {
		cli_event_queue_attach_app(ctx, &ctx->base);
		app_rc = nt_application_set_color_profile(&ctx->base, cli_output_color_enabled(ctx)
			? NT_COLOR_PROFILE_TRUECOLOR : NT_COLOR_PROFILE_NONE);
		if (!app_rc)
			app_rc = nt_window_set_selection_handler(nt_application_window(&ctx->base),
			cli_application_copy_selection, ctx);

		if (!app_rc)
			app_rc = cli_chat_present(ctx);

		if (!app_rc)
			app_rc = nt_application_run(&ctx->base);

		nb_log_write(ctx->logger, NB_LOG_DEBUG, "TUI event loop finished with status %d",
			     app_rc);

		nt_application_deinit(&ctx->base);
		cli_feed_set_hidden(ctx, 0);
	}

	if (app_rc < 0)
		CMD_ERROR(ctx, "Application failed: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, app_rc));
}

static void cli_application_run_interactive(struct cli_application *ctx)
{
	struct cli_command_job job;
	int rc = cli_command_job_init(&job);

	if (rc) {
		CMD_ERROR(ctx, "failed to initialize command worker: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return;
	}

	ctx->input_job = &job;
	ctx->input_status.enabled = 1;
	if (cli_composer_model_init(&ctx->composer) == 0) {
		cli_application_run_session(ctx);
		cli_composer_model_cleanup(&ctx->composer);
	}

	ctx->input_job = NULL;
	ctx->input_status.enabled = 0;
	cli_command_job_cleanup(&job);
	if (!ctx->pending_db_restore)
		cli_session_history_print_resume_hint(ctx);
}

struct nt_application *cli_application_tui_borrow(struct cli_application *ctx)
{
	return ctx && ctx->base.state ? &ctx->base : NULL;
}

int cli_application_flush_output(struct cli_application *ctx)
{
	if (ctx && ctx->output.capture)
		return 0;

	if (ctx && ctx->events && !cli_event_queue_is_owner(ctx)) {
		cli_event_queue_notify(ctx);
		return 0;
	}

	struct nt_application *app = cli_application_tui_borrow(ctx);
	if (app)
		return nt_application_render(app);

	return fflush(stdout) == 0 ? 0 : NB_ERR_IO;
}

static int cli_application_initialize(struct cli_application *ctx, const char *config_path,
				      const char *db_path, const char *workdir,
				      const char *session_name, const char *resume_ref,
				      enum cli_presentation_mode mode)
{
	struct runtime_options options;
	int rc;

	if (!ctx)
		return NB_ERR_INVAL;

	ctx->presentation_mode = mode;
	cli_output_set_structured(ctx, mode == CLI_PRESENT_EVENTS_JSON);
	ctx->event_cb = cli_runtime_events_callback;
	ctx->event_user_data = ctx;
	rc = cli_input_init(ctx);
	if (rc)
		return rc;

	rc = cli_feed_init(ctx, stdout, STDOUT_FILENO);
	if (rc) {
		cli_input_cleanup(ctx);
		return rc;
	}

	rc = cli_presentation_init(ctx);
	if (rc != 0) {
		cli_feed_cleanup(ctx);
		cli_input_cleanup(ctx);
		return rc;
	}

	rc = cli_event_queue_init(ctx, cli_dispatch_event);
	if (rc != 0) {
		cli_subagents_cleanup(ctx);
		cli_presentation_cleanup(ctx);
		cli_feed_cleanup(ctx);
		cli_input_cleanup(ctx);
		return rc;
	}

	if (mode == CLI_PRESENT_INTERACTIVE)
		(void)cli_event_queue_set_events_enabled(ctx, 0);

	memset(&options, 0, sizeof(options));
	options.logger = ctx->logger;
	options.startup_error = &ctx->startup_error;
	options.config_path = config_path ? config_path : "~/.morph/config.toml";
	options.db_path = db_path ? db_path : "~/.morph/data.db";
	options.workdir = workdir;
	options.default_session = session_name;
	options.resume_session_ref = resume_ref;
	options.front_name = "cli";
	options.event_cb = ctx->event_cb;
	options.event_user_data = ctx;
	options.usage_observer = cli_runtime_observer_usage;
	options.usage_observer_user_data = ctx;
	options.hitl_cb = cli_approval_tool_callback;
	options.hitl_user_data = ctx;
	options.ask_user_cb = cli_question_callback;
	options.ask_user_user_data = ctx;
	options.operation_approval_cb = cli_approval_operation_callback;
	options.operation_approval_user_data = ctx;
	options.img_annotate_pause_cb = cli_runtime_observer_pause;
	options.img_annotate_resume_cb = cli_runtime_observer_resume;
	options.img_annotate_user_data = ctx;
	options.enable_exec = 1;
	options.enable_apply_patch = 1;
	options.enable_config_write = 1;
	options.enable_img_annotate = 1;
	options.enable_shell_exts = 1;
	options.enable_multi_agent = 1;
	options.allocate_skill_registry = 1;
	options.auto_connect_mcp = 1;
	options.background_connect_mcp = ctx->presentation_mode == CLI_PRESENT_INTERACTIVE;
	options.create_new_session =
		(session_name && session_name[0]) || (resume_ref && resume_ref[0]) ? 0 : 1;
	rc = runtime_open(&options, &ctx->runtime);
	if (rc != 0) {
		cli_event_queue_cleanup(ctx);
		cli_subagents_cleanup(ctx);
		cli_presentation_cleanup(ctx);
		cli_feed_cleanup(ctx);
		cli_input_cleanup(ctx);
		return rc;
	}

	(void)cli_agents_refresh(ctx);

	ctx->session_auto_named = session_name && session_name[0] ? 1 : 0;
	ctx->session_history_pending = ctx->session_auto_named;
	ctx->running = 1;
	ctx->presentation_ready = 1;
	ctx->image_path[0] = '\0';
	{
		const char *resolved_workdir = runtime_workdir_borrow(ctx->runtime);

		if (resolved_workdir && resolved_workdir[0])
			strncpy(ctx->workdir, resolved_workdir, sizeof(ctx->workdir) - 1);
	}

	rc = cli_sessions_init(ctx);
	if (rc) {
		cli_application_shutdown(ctx);
		return rc;
	}
	rc = runtime_sync_start_instance(ctx->runtime, NULL, NULL);
	if (rc != 0)
		nb_log_write(ctx ? ctx->logger : NULL, NB_LOG_WARN,
			     "failed to start sync worker: %s",
			     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));

	return 0;
}

int cli_application_init(struct cli_application *ctx, struct nb_log *logger,
			 const char *config_path, const char *db_path, const char *workdir,
			 const char *session_name, const char *resume_ref,
			 enum cli_presentation_mode mode)
{
	if (!ctx)
		return NB_ERR_INVAL;

	memset(ctx, 0, sizeof(*ctx));
	ctx->input_execute = cli_command_input_execute;
	ctx->logger = logger;
	int rc = nb_signal_listener_start(&ctx->signals,
					  (const int[]){SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGWINCH},
					  5, cli_signal_received, ctx, NULL);

	if (rc == 0) {
		rc = cli_application_initialize(ctx, config_path, db_path, workdir, session_name,
						resume_ref, mode);
		if (rc != 0) {
			nb_signal_listener_stop(&ctx->signals, NULL);
		} else {
			__atomic_store_n(&ctx->signals_ready, 1, __ATOMIC_RELEASE);
			if (nb_atomic_int_load(&ctx->interrupted)) {
				cli_application_shutdown(ctx);
				return NB_ERR_INTERRUPTED;
			}
		}
	}

	return rc;
}

#define CLI_APPLICATION_STRUCTURED_POLL_TIMEOUT_MS 100

/* Application execution modes. */

static void cli_application_run_structured(struct cli_application *ctx)
{
	char line[BUFSIZ];
	nb_buf input NB_CLEANUP(nb_buf_cleanup) = {0};
	struct cli_command_job job;
	int rc;

	rc = cli_command_job_init(&job);
	if (rc != 0)
		return;

	rc = (nb_buf_init(&input), nb_buf_reserve(&input, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	if (rc != 0) {
		cli_command_job_cleanup(&job);
		return;
	}

	while (ctx->running || job.active) {
		if (nb_atomic_int_load(&ctx->terminate_requested)) {
			ctx->running = 0;
			if (!job.active)
				break;
		}

		struct pollfd fds[2];
		int wake_fd = cli_event_queue_wake_fd(ctx);
		int nfds = wake_fd >= 0 ? 2 : 1;
		int poll_timeout = job.active ? CLI_APPLICATION_STRUCTURED_POLL_TIMEOUT_MS : -1;

		fds[0].fd = job.active ? -1 : STDIN_FILENO;
		fds[0].events = POLLIN;
		fds[0].revents = 0;
		if (wake_fd >= 0) {
			fds[1].fd = wake_fd;
			fds[1].events = POLLIN;
			fds[1].revents = 0;
		}

		rc = poll(fds, (nfds_t)nfds, poll_timeout);
		if (rc < 0) {
			if (errno == EINTR) {
				nb_atomic_int_store(&ctx->interrupted, 0);
				continue;
			}

			break;
		}

		if ((wake_fd >= 0 && (fds[1].revents & (POLLIN | POLLERR | POLLHUP))) ||
		    (job.active && rc == 0))
			(void)cli_event_queue_drain(ctx);

		if (cli_command_job_done(&job)) {
			int command_rc = cli_command_job_finish(&job);

			(void)cli_event_queue_drain(ctx);
			cli_turn_finish(ctx, command_rc);
		}

		if (!job.active && (fds[0].revents & (POLLIN | POLLHUP))) {
			size_t len;
			int complete = 0;

			nb_buf_clear(&input);
			while (!complete && ctx->running) {
				int continuation;

				cli_cancel_state_reset(ctx);
				if (!cli_input_fgets(ctx, line, sizeof(line), stdin)) {
					if (feof(stdin))
						ctx->running = 0;
					else
						clearerr(stdin);

					break;
				}

				len = strlen(line);
				complete = len > 0 && line[len - 1] == '\n';
				if (complete)
					line[--len] = '\0';

				if (len > 0 && line[len - 1] == '\r')
					line[--len] = '\0';

				continuation = complete && len > 0 && line[len - 1] == '\\';
				if (continuation) {
					len--;
					complete = 0;
				}

				rc = (nb_buf_append(&input, line, len) ? 0 : NB_ERR_NOMEM);
				if (rc == 0 && continuation)
					rc = (nb_buf_append_char(&input, '\n') ? 0 : NB_ERR_NOMEM);

				if (rc != 0) {
					nb_buf_clear(&input);
					break;
				}
			}

			if (input.len > 0 && !cli_interaction_is_frame(nb_buf_cstr(&input))) {
				cli_turn_begin(ctx);
				rc = cli_command_job_start(&job, ctx, nb_buf_cstr(&input));
				if (rc != 0)
					cli_turn_finish(ctx, rc);
			}
		}
	}

	if (job.active) {
		int command_rc = cli_command_job_finish(&job);

		(void)cli_event_queue_drain(ctx);
		cli_turn_finish(ctx, command_rc);
	}

	nb_buf_cleanup(&input);
	cli_command_job_cleanup(&job);
}

static void cli_application_run_plain(struct cli_application *ctx)
{
	nb_buf input NB_CLEANUP(nb_buf_cleanup) = {0};
	struct cli_command_job job;
	char line[BUFSIZ];
	int prompt = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

	if (cli_command_job_init(&job) != 0)
		return;

	if ((nb_buf_init(&input), nb_buf_reserve(&input, BUFSIZ) ? 0 : NB_ERR_NOMEM) != 0) {
		cli_command_job_cleanup(&job);
		return;
	}

	while (ctx->running) {
		if (nb_atomic_int_load(&ctx->terminate_requested))
			break;

		int complete = 0;

		(void)cli_event_queue_drain(ctx);
		nb_buf_clear(&input);
		while (!complete) {
			size_t len;
			int has_newline;
			int continuation;

			if (prompt) {
				cli_output_printf(ctx, "%s", input.len == 0 ? "> " : "… ");
				fflush(stdout);
			}

			cli_cancel_state_reset(ctx);
			if (!cli_input_fgets(ctx, line, sizeof(line), stdin)) {
				if (nb_atomic_int_exchange(&ctx->interrupted, 0)) {
					clearerr(stdin);
					nb_buf_clear(&input);
					break;
				}

				if (feof(stdin)) {
					ctx->running = 0;
					break;
				}

				clearerr(stdin);
				continue;
			}

			len = strlen(line);
			has_newline = len > 0 && line[len - 1] == '\n';
			if (has_newline)
				line[--len] = '\0';

			if (len > 0 && line[len - 1] == '\r')
				line[--len] = '\0';

			continuation = has_newline && len > 0 && line[len - 1] == '\\';
			if (continuation)
				len--;

			if ((nb_buf_append(&input, line, len) ? 0 : NB_ERR_NOMEM) != 0) {
				CMD_ERROR(ctx, "input is too large");
				nb_buf_clear(&input);
				break;
			}

			if (continuation) {
				if ((nb_buf_append_char(&input, '\n') ? 0 : NB_ERR_NOMEM) != 0) {
					CMD_ERROR(ctx, "input is too large");
					nb_buf_clear(&input);
					break;
				}
			} else if (has_newline) {
				complete = 1;
			}
		}

		if (input.len > 0) {
			int turn_rc;

			cli_turn_begin(ctx);
			turn_rc = cli_command_job_start(&job, ctx, nb_buf_cstr(&input));
			if (turn_rc == 0)
				turn_rc = cli_command_job_wait(&job);

			(void)cli_event_queue_drain(ctx);
			cli_turn_finish(ctx, turn_rc);
		}

		(void)cli_event_queue_drain(ctx);
	}

	nb_buf_cleanup(&input);
	cli_command_job_cleanup(&job);
}

void cli_application_run(struct cli_application *ctx)
{
	const struct config *config;
	const char *workdir;
	const char *home;
	const char *term = getenv("TERM");
	int plain = !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
		    (term && strcmp(term, "dumb") == 0);
	int standalone_resume = plain;
	nb_buf directory NB_CLEANUP(nb_buf_cleanup) = {0};

	if (!ctx)
		return;

	if (cli_scheduler_start(ctx) != 0)
		nb_log_write(ctx ? ctx->logger : NULL, NB_LOG_WARN,
			     "failed to start task scheduler");

	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON) {
		(void)setvbuf(stdin, NULL, _IONBF, 0);
		cli_application_run_structured(ctx);
		return;
	}

	if (plain) {
		(void)cli_event_queue_set_events_enabled(ctx, 1);
		cli_output_set_color_enabled(ctx, 0);
		ctx->presentation_mode = CLI_PRESENT_ONCE_PLAIN;
	}

	config = runtime_config_borrow(ctx->runtime);
	workdir = runtime_workdir_borrow(ctx->runtime);
	memset(&directory, 0, sizeof(directory));
	if ((nb_buf_init(&directory), nb_buf_reserve(&directory, BUFSIZ) ? 0 : NB_ERR_NOMEM) != 0)
		return;

	home = getenv("HOME");
	if (workdir && workdir[0] && home && home[0] && strncmp(workdir, home, strlen(home)) == 0 &&
	    (workdir[strlen(home)] == '\0' || workdir[strlen(home)] == '/')) {
		(void)nb_buf_append_char(&directory, '~');
		(void)nb_buf_append_cstr(&directory, workdir + strlen(home));
	} else {
		(void)nb_buf_append_cstr(&directory, workdir && workdir[0] ? workdir : ".");
	}

	if (plain && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))
		cli_banner_print(ctx, config ? config->models.text.model : NULL,
				 nb_buf_cstr(&directory));

	nb_buf_cleanup(&directory);
	int hooks_rc = cli_hooks_startup(ctx);
	if (hooks_rc != 0)
		nb_log_write(ctx ? ctx->logger : NULL, NB_LOG_WARN, "hook trust review failed: %s",
			     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, hooks_rc));

	if (plain && ctx->session_history_pending) {
		ctx->session_history_pending = 0;
		int rc = cli_session_history_restore(ctx);

		if (rc)
			CMD_ERROR(ctx, "failed to restore session history: %s",
				  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	}

	/* The chat controller opens the startup picker on its own controller stack. */
	if (ctx->resume_requested && standalone_resume) {
		ctx->resume_requested = 0;
		int rc = cli_resume_show(ctx);

		if (rc)
			CMD_ERROR(ctx, "failed to open sessions: %s",
				  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	}

	if (plain) {
		cli_application_run_plain(ctx);

		return;
	}

	cli_application_run_interactive(ctx);
}

static void cli_application_emit_trace_json(struct cli_application *ctx, double elapsed)
{
	if (ctx->sessions) {
		cJSON *trace NB_CLEANUP(cJSON_DeleteOwner) = NULL;
		if (!cli_sessions_request_current(ctx, "turn.inspect", NULL, &trace)) {
			cJSON_AddNumberToObject(trace, "elapsed_seconds", elapsed);
			char *text NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(trace);
			if (text)
				cli_output_printf(ctx, "%s\n", text);

			return;
		}
	}

	struct runtime_turn_status status NB_CLEANUP(runtime_turn_status_cleanup) = {0};
	cJSON *root;
	cJSON *steps;
	char *json;

	if (runtime_turn_status_copy(ctx->runtime, &status) != 0)
		return;

	root = cJSON_CreateObject();
	if (!root) {
		return;
	}

	switch (status.state) {
	case REACT_STATE_DONE:
		cJSON_AddStringToObject(root, "state", "done");
		break;
	case REACT_STATE_ABORT:
		cJSON_AddStringToObject(root, "state", "abort");
		break;
	case REACT_STATE_TOOL_FAIL:
		cJSON_AddStringToObject(root, "state", "tool_fail");
		break;
	default:
		cJSON_AddStringToObject(root, "state", "unknown");
		break;
	}

	cJSON_AddStringToObject(root, "final_answer",
				status.final_answer ? status.final_answer : "");
	cJSON_AddStringToObject(root, "outcome", react_outcome_name(status.outcome));
	if (status.last_error_code < 0) {
		cJSON_AddNumberToObject(root, "error_code", status.last_error_code);
		cJSON_AddStringToObject(root, "error",
					nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
						   status.last_error_code));
	}

	if (status.outcome_reason && status.outcome_reason[0])
		cJSON_AddStringToObject(root, "reason", status.outcome_reason);

	steps = cJSON_CreateArray();
	if (steps) {
		for (int i = 0; i < status.step_count; i++) {
			struct runtime_trace_step *cur = &status.steps[i];
			cJSON *step = cJSON_CreateObject();

			if (!step)
				continue;

			cJSON_AddStringToObject(step, "type", react_step_type_name(cur->type));
			if (cur->content)
				cJSON_AddStringToObject(step, "content", cur->content);

			if (cur->tool_name)
				cJSON_AddStringToObject(step, "tool_name", cur->tool_name);

			if (cur->tool_args)
				cJSON_AddStringToObject(step, "tool_args", cur->tool_args);

			cJSON_AddItemToArray(steps, step);
		}

		cJSON_AddItemToObject(root, "steps", steps);
	}

	cJSON_AddNumberToObject(root, "elapsed_seconds", elapsed);
	json = cJSON_PrintUnformatted(root);
	if (json) {
		cli_output_printf(ctx, "%s\n", json);
		free(json);
	}

	cJSON_Delete(root);
}

void cli_application_run_once(struct cli_application *ctx, const char *prompt)
{
	struct cli_command_job job;
	int64_t started;
	int64_t finished;
	int saved_stdout = -1;
	int job_initialized = 0;
	int job_rc;
	double elapsed;

	if (!ctx || !prompt)
		return;

	started = nb_time_monotonic_ns();
	cli_cancel_state_reset(ctx);

	/*
	 * Trace mode keeps its historical contract: progress goes to stderr
	 * and the single aggregate JSON document is written to stdout.
	 */
	if (ctx->trace_json) {
		saved_stdout = dup(STDOUT_FILENO);
		if (saved_stdout >= 0)
			(void)dup2(STDERR_FILENO, STDOUT_FILENO);
	}

	cli_turn_begin(ctx);
	job_rc = cli_command_job_init(&job);
	if (job_rc == 0) {
		job_initialized = 1;
		job_rc = cli_command_job_start(&job, ctx, prompt);
	}

	if (job_rc == 0)
		job_rc = cli_command_job_wait(&job);

	(void)cli_event_queue_drain(ctx);
	cli_turn_finish(ctx, job_rc);
	if (job_initialized)
		cli_command_job_cleanup(&job);

	if (saved_stdout >= 0) {
		fflush(stdout);
		(void)dup2(saved_stdout, STDOUT_FILENO);
		close(saved_stdout);
	}

	finished = nb_time_monotonic_ns();
	elapsed = started > 0 && finished >= started ? (double)(finished - started) / 1e9 : 0.0;
	if (ctx->trace_json)
		cli_application_emit_trace_json(ctx, elapsed);
}

/* Application resources outlive pages and terminal teardown. */

void cli_application_shutdown(struct cli_application *ctx)
{
	if (!ctx)
		return;

	if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
		(void)cli_event_queue_set_events_enabled(ctx, 0);
		ctx->presentation_ready = 0;
	}

	nb_signal_listener_stop(&ctx->signals, NULL);
	cli_callbacks_cleanup(ctx);
	cli_sessions_stop(ctx);
	cli_sessions_cleanup(ctx);
	runtime_close(ctx->runtime);
	ctx->runtime = NULL;
	(void)cli_event_queue_drain(ctx);
	cli_subagents_cleanup(ctx);
	cli_presentation_cleanup(ctx);
	cli_composer_model_cleanup(&ctx->composer);
	cli_jobs_model_cleanup(ctx->jobs);
	ctx->jobs = NULL;
	cli_event_queue_cleanup(ctx);
	cli_feed_cleanup(ctx);
	cli_input_cleanup(ctx);
	cli_output_set_structured(ctx, 0);
	nb_log_write(ctx ? ctx->logger : NULL, NB_LOG_INFO, "cli shutdown complete");
}
