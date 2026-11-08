#include "sapi/cli/application.h"
#include "util/cleanup.h"
#include "sapi/cli/adapters/input/state.h"
#include "sapi/cli/adapters/runtime/dispatch.h"
#include "sapi/cli/services/turn.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/pages/resume.h"
#include "sapi/cli/services/session_history.h"
#include "sapi/cli/adapters/runtime/observer.h"
#include <stdio.h>
#include "sapi/cli/services/command_job.h"
#include "sapi/cli/adapters/input/callbacks.h"
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
#include "credits.h"
#include "config/config.h"
#include "sapi/cli/pages/question.h"
#include "sapi/cli/adapters/runtime/events.h"
#include "sapi/cli/services/scheduler.h"
#include "sapi/cli/pages/approval.h"
#include "sapi/cli/adapters/input/reader.h"
#include "sapi/cli/widgets/banner.h"
#include "sapi/cli/pages/hooks.h"
#include "sapi/cli/adapters/protocol/interaction.h"
#include "cJSON.h"

struct cli_application_screen {
	struct cli_application *ctx;
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

int cli_application_flush(struct cli_application *ctx)
{
	morph_buf_t *pending = ctx->application_output;

	if (pending && pending->failed)
		return pending->failed;
	if (pending && pending->len) {
		if (fwrite(morph_buf_cstr_borrow(pending), 1, pending->len, stdout) !=
			    pending->len ||
		    fflush(stdout) != 0)
			return errno ? -errno : -EIO;
		morph_buf_reset(pending);
	}
	return 0;
}

void cli_application_panel_complete(struct cli_application *ctx, void *data,
				    void (*complete)(void *, int), int result)
{
	if (morph_tui_application_depth(cli_application_tui_borrow(ctx)) <= 1) {
		int rc = cli_application_flush(ctx);

		if (rc) {
			result = rc;
			morph_tui_application_stop(cli_application_tui_borrow(ctx), rc);
		}
	}
	complete(data, result);
	if (ctx->application_command &&
	    morph_tui_application_depth(cli_application_tui_borrow(ctx)) <= 1) {
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

int cli_application_present_screen(struct cli_application *ctx,
				   const struct morph_tui_screen_ops *ops, void *data,
				   void (*complete)(void *data, int result))
{
	if (!ctx || !cli_application_tui_borrow(ctx) || !ops || !ops->update || !ops->draw ||
	    !complete)
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
	int rc = morph_tui_application_present_controller_take(cli_application_tui_borrow(ctx),
							       &controller);

	if (rc)
		free(screen);
	return rc;
}

#ifdef HAVE_READLINE
static void cli_application_prepare_termination(struct morph_tui_application *base, int result)
{
	struct cli_application *ctx = (struct cli_application *)base;

	(void)result;
	ctx->running = 0;
	(void)cli_event_queue_stop_owner_calls(ctx);
	if (ctx->input_job && ctx->input_job->active) {
		runtime_cancel_turn(ctx->runtime);
		cli_chat_job_finished(ctx->chat, cli_command_job_finish(ctx->input_job));
	}
	cli_event_queue_attach_app(ctx, NULL);
}

static void cli_application_run_session(struct cli_application *ctx, struct cli_command_job *job)
{
	struct cli_chat_page *page = NULL;
	int app_rc = cli_chat_create(ctx, job, &page);

	if (app_rc) {
		CMD_ERROR(ctx, "Application failed: %s", morph_strerror(app_rc));
		return;
	}
	const struct morph_tui_application_options options = cli_chat_options(page);
	struct morph_tui_controller controller = cli_chat_controller(page);
	static const struct morph_tui_application_ops methods = {
		.prepare_termination = cli_application_prepare_termination,
	};

	ctx->base.ops = &methods;
	app_rc = morph_tui_application_init(&ctx->base, &options);
	if (!app_rc) {
		cli_event_queue_attach_app(ctx, &ctx->base);
		app_rc = morph_tui_application_present_controller_take(&ctx->base, &controller);
		if (!app_rc)
			app_rc = morph_tui_application_run(&ctx->base);
		morph_tui_application_deinit(&ctx->base);
		cli_feed_set_hidden(ctx, 0);
		int flush_rc = cli_application_flush(ctx);

		if (!app_rc)
			app_rc = flush_rc;
	}
	ctx->application_output = NULL;
	cli_chat_destroy(page, app_rc);
}

static void cli_application_run_interactive(struct cli_application *ctx)
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

struct morph_tui_application *cli_application_tui_borrow(struct cli_application *ctx)
{
	return ctx && ctx->base.state ? &ctx->base : NULL;
}

static int cli_application_initialize(struct cli_application *ctx, const char *config_path,
				      const char *db_path, const char *workdir,
				      const char *session_name, enum cli_presentation_mode mode)
{
	struct runtime_options options;
	int rc;

	if (!ctx)
		return -EINVAL;
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
		cli_presentation_cleanup(ctx);
		cli_feed_cleanup(ctx);
		cli_input_cleanup(ctx);
		return rc;
	}
	memset(&options, 0, sizeof(options));
	options.logger = ctx->logger;
	options.startup_error = &ctx->startup_error;
	options.config_path = config_path ? config_path : "~/.morph/config.toml";
	options.db_path = db_path ? db_path : "~/.morph/data.db";
	options.workdir = workdir;
	options.default_session = session_name;
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
	options.enable_sub_agents = 1;
	options.allocate_skill_registry = 1;
	options.auto_connect_mcp = 1;
	options.background_connect_mcp = ctx->presentation_mode == CLI_PRESENT_INTERACTIVE;
	options.create_new_session = session_name && session_name[0] ? 0 : 1;
	rc = runtime_open(&options, &ctx->runtime);
	if (rc != 0) {
		cli_event_queue_cleanup(ctx);
		cli_presentation_cleanup(ctx);
		cli_feed_cleanup(ctx);
		cli_input_cleanup(ctx);
		return rc;
	}

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
	rc = runtime_sync_start_instance(ctx->runtime, NULL, NULL);
	if (rc != 0)
		log_warn(ctx ? ctx->logger : NULL, "failed to start sync worker: %s",
			 morph_strerror(rc));
	return 0;
}

int cli_application_init(struct cli_application *ctx, struct morph_log *logger,
			 const char *config_path, const char *db_path, const char *workdir,
			 const char *session_name, enum cli_presentation_mode mode)
{
	if (!ctx)
		return -EINVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->logger = logger;
	int rc = morph_signal_listener_start(&ctx->signals, cli_signal_received, ctx);

	if (rc == 0) {
		rc = cli_application_initialize(ctx, config_path, db_path, workdir, session_name,
						mode);
		if (rc != 0) {
			morph_signal_listener_stop(&ctx->signals);
		} else {
			__atomic_store_n(&ctx->signals_ready, 1, __ATOMIC_RELEASE);
			if (morph_signal_flag_get(&ctx->interrupted)) {
				cli_application_shutdown(ctx);
				return -EINTR;
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
	morph_buf_t input;
	struct cli_command_job job;
	int rc;

	rc = cli_command_job_init(&job);
	if (rc != 0)
		return;
	rc = morph_buf_init(&input, BUFSIZ);
	if (rc != 0) {
		cli_command_job_cleanup(&job);
		return;
	}
	while (ctx->running || job.active) {
		if (morph_signal_flag_get(&ctx->terminate_requested)) {
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
				morph_signal_flag_set(&ctx->interrupted, 0);
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

			morph_buf_reset(&input);
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
				rc = morph_buf_append(&input, line, len);
				if (rc == 0 && continuation)
					rc = morph_buf_putc(&input, '\n');
				if (rc != 0) {
					morph_buf_reset(&input);
					break;
				}
			}
			if (input.len > 0 &&
			    !cli_interaction_is_frame(morph_buf_cstr_borrow(&input))) {
				cli_turn_begin(ctx);
				rc = cli_command_job_start(&job, ctx,
							   morph_buf_cstr_borrow(&input));
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
	morph_buf_cleanup(&input);
	cli_command_job_cleanup(&job);
}

static void cli_application_run_plain(struct cli_application *ctx)
{
	morph_buf_t input;
	struct cli_command_job job;
	char line[BUFSIZ];
	int prompt = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

	if (cli_command_job_init(&job) != 0)
		return;
	if (morph_buf_init(&input, BUFSIZ) != 0) {
		cli_command_job_cleanup(&job);
		return;
	}
	while (ctx->running) {
		if (morph_signal_flag_get(&ctx->terminate_requested))
			break;
		int complete = 0;

		(void)cli_event_queue_drain(ctx);
		morph_buf_reset(&input);
		while (!complete) {
			size_t len;
			int has_newline;
			int continuation;

			if (prompt) {
				printf(ctx, "%s", input.len == 0 ? "> " : "… ");
				fflush(stdout);
			}
			cli_cancel_state_reset(ctx);
			if (!cli_input_fgets(ctx, line, sizeof(line), stdin)) {
				if (morph_signal_flag_take(&ctx->interrupted)) {
					clearerr(stdin);
					morph_buf_reset(&input);
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
			if (morph_buf_append(&input, line, len) != 0) {
				CMD_ERROR(ctx, "input is too large");
				morph_buf_reset(&input);
				break;
			}
			if (continuation) {
				if (morph_buf_putc(&input, '\n') != 0) {
					CMD_ERROR(ctx, "input is too large");
					morph_buf_reset(&input);
					break;
				}
			} else if (has_newline) {
				complete = 1;
			}
		}
		if (input.len > 0) {
			int turn_rc;

			cli_turn_begin(ctx);
			turn_rc = cli_command_job_start(&job, ctx, morph_buf_cstr_borrow(&input));
			if (turn_rc == 0)
				turn_rc = cli_command_job_wait(&job);
			(void)cli_event_queue_drain(ctx);
			cli_turn_finish(ctx, turn_rc);
		}
		(void)cli_event_queue_drain(ctx);
	}
	morph_buf_cleanup(&input);
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
	morph_buf_t directory;

#ifndef HAVE_READLINE
	standalone_resume = 1;
#endif
	if (!ctx)
		return;
	if (cli_scheduler_start(ctx) != 0)
		log_warn(ctx ? ctx->logger : NULL, "failed to start task scheduler");
	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON) {
		(void)setvbuf(stdin, NULL, _IONBF, 0);
		cli_application_run_structured(ctx);
		return;
	}
	if (plain) {
		cli_output_set_color_enabled(ctx, 0);
		ctx->presentation_mode = CLI_PRESENT_ONCE_PLAIN;
	}
	config = runtime_config_borrow(ctx->runtime);
	workdir = runtime_workdir_borrow(ctx->runtime);
	memset(&directory, 0, sizeof(directory));
	if (morph_buf_init(&directory, BUFSIZ) != 0)
		return;
	home = getenv("HOME");
	if (workdir && workdir[0] && home && home[0] && strncmp(workdir, home, strlen(home)) == 0 &&
	    (workdir[strlen(home)] == '\0' || workdir[strlen(home)] == '/')) {
		(void)morph_buf_putc(&directory, '~');
		(void)morph_buf_puts(&directory, workdir + strlen(home));
	} else {
		(void)morph_buf_puts(&directory, workdir && workdir[0] ? workdir : ".");
	}
	if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))
		cli_banner_print(ctx, config ? config->models.text.model : NULL,
				 morph_buf_cstr_borrow(&directory));
	morph_buf_cleanup(&directory);
	int hooks_rc = cli_hooks_startup(ctx);
	if (hooks_rc != 0)
		log_warn(ctx ? ctx->logger : NULL, "hook trust review failed: %s",
			 morph_strerror(hooks_rc));
	if (ctx->session_history_pending) {
		ctx->session_history_pending = 0;
		int rc = cli_session_history_restore(ctx);

		if (rc)
			CMD_ERROR(ctx, "failed to restore session history: %s", morph_strerror(rc));
	}
	/* The chat controller opens the startup picker on its own controller stack. */
	if (ctx->resume_requested && standalone_resume) {
		ctx->resume_requested = 0;
		int rc = cli_resume_show(ctx);

		if (rc)
			CMD_ERROR(ctx, "failed to open sessions: %s", morph_strerror(rc));
	}

	if (plain) {
		cli_application_run_plain(ctx);

		return;
	}

#ifdef HAVE_READLINE
	cli_application_run_interactive(ctx);
#else
	cli_application_run_plain(ctx);
#endif
}

static void cli_application_emit_trace_json(struct cli_application *ctx, double elapsed)
{
	struct runtime_turn_status status MORPH_CLEANUP(runtime_turn_status_cleanup) = {0};
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
		cJSON_AddStringToObject(root, "error", morph_strerror(status.last_error_code));
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
		printf(ctx, "%s\n", json);
		free(json);
	}
	cJSON_Delete(root);
}

void cli_application_run_once(struct cli_application *ctx, const char *prompt)
{
	struct cli_command_job job;
	struct timespec started;
	struct timespec finished;
	int saved_stdout = -1;
	int job_initialized = 0;
	int job_rc;
	double elapsed;

	if (!ctx || !prompt)
		return;
	clock_gettime(CLOCK_MONOTONIC, &started);
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
	clock_gettime(CLOCK_MONOTONIC, &finished);
	elapsed = (double)(finished.tv_sec - started.tv_sec) +
		  (double)(finished.tv_nsec - started.tv_nsec) / 1e9;
	if (ctx->trace_json)
		cli_application_emit_trace_json(ctx, elapsed);
}

/* Application resources outlive pages and terminal teardown. */

void cli_application_shutdown(struct cli_application *ctx)
{
	if (!ctx)
		return;
	morph_signal_listener_stop(&ctx->signals);
	cli_callbacks_cleanup(ctx);
	runtime_close(ctx->runtime);
	ctx->runtime = NULL;
	(void)cli_event_queue_drain(ctx);
	cli_presentation_cleanup(ctx);
	cli_event_queue_cleanup(ctx);
	cli_feed_cleanup(ctx);
	cli_input_cleanup(ctx);
	cli_output_set_structured(ctx, 0);
	log_info(ctx ? ctx->logger : NULL, "cli shutdown complete");
}
