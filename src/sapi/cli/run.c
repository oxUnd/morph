#include "sapi/cli/command_job.h"
#include "config/config.h"
#include "sapi/cli/ui/input/reader.h"
#include "sapi/cli/ui/input/cancel.h"
#include "sapi/cli/ui/presentation/banner.h"
#include "sapi/cli/scheduler.h"
#include "sapi/cli/core.h"
#include "sapi/cli/ui/event_queue.h"
#include "sapi/cli/internal.h"
#include "sapi/cli/ui/application.h"
#include "sapi/cli/ui/screens/hooks.h"
#include "sapi/cli/interaction.h"

#define CLI_STRUCTURED_POLL_TIMEOUT_MS 100

/* ---- cli_run ---- */

static void cli_run_structured(struct cli_context *ctx)
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
		int poll_timeout = job.active ?
			CLI_STRUCTURED_POLL_TIMEOUT_MS : -1;

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
		if ((wake_fd >= 0 &&
		     (fds[1].revents & (POLLIN | POLLERR | POLLHUP))) ||
		    (job.active && rc == 0))
			(void)cli_event_queue_drain(ctx);
		if (cli_command_job_done(&job)) {
			int command_rc = cli_command_job_finish(&job);

			(void)cli_event_queue_drain(ctx);
			cli_turn_finish(ctx, command_rc);
		}
		if (!job.active &&
		    (fds[0].revents & (POLLIN | POLLHUP))) {
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
				continuation = complete && len > 0 &&
					line[len - 1] == '\\';
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
			if (input.len > 0 && !cli_interaction_is_frame(morph_buf_cstr(&input))) {
				cli_turn_begin(ctx);
				rc = cli_command_job_start(&job, ctx,
					morph_buf_cstr(&input));
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

static void cli_run_plain(struct cli_context *ctx)
{
	morph_buf_t input;
	char line[BUFSIZ];
	int prompt = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

	if (morph_buf_init(&input, BUFSIZ) != 0)
		return;
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
			continuation = has_newline && len > 0 &&
				line[len - 1] == '\\';
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
			turn_rc = cli_handle_command(ctx, morph_buf_cstr(&input));
			(void)cli_event_queue_drain(ctx);
			cli_turn_finish(ctx, turn_rc);
		}
		(void)cli_event_queue_drain(ctx);
	}
	morph_buf_cleanup(&input);
}

void cli_run(struct cli_context *ctx)
{
	const struct config *config;
	const char *workdir;
	const char *home;
	const char *term = getenv("TERM");
	int plain = !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
		(term && strcmp(term, "dumb") == 0);
	morph_buf_t directory;

	if (!ctx)
		return;
	if (cli_scheduler_start(ctx) != 0)
		log_warn(ctx ? ctx->logger : NULL, "failed to start task scheduler");
	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON) {
		(void)setvbuf(stdin, NULL, _IONBF, 0);
		cli_run_structured(ctx);
		return;
	}
	if (plain) {
		cli_set_color_enabled(ctx, 0);
		ctx->presentation_mode = CLI_PRESENT_ONCE_PLAIN;
	}
	config = runtime_config_get(ctx->runtime);
	workdir = runtime_workdir_get(ctx->runtime);
	memset(&directory, 0, sizeof(directory));
	if (morph_buf_init(&directory, BUFSIZ) != 0)
		return;
	home = getenv("HOME");
	if (workdir && workdir[0] && home && home[0] &&
	    strncmp(workdir, home, strlen(home)) == 0 &&
	    (workdir[strlen(home)] == '\0' ||
	     workdir[strlen(home)] == '/')) {
		(void)morph_buf_putc(&directory, '~');
		(void)morph_buf_puts(&directory, workdir + strlen(home));
	} else {
		(void)morph_buf_puts(&directory,
				    workdir && workdir[0] ? workdir : ".");
	}
	if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))
		cli_banner_print(ctx, config ? config->models.text.model : NULL,
				 morph_buf_cstr(&directory));
	morph_buf_cleanup(&directory);
	int hooks_rc = cli_hooks_startup(ctx);
	if (hooks_rc != 0)
		log_warn(ctx ? ctx->logger : NULL, "hook trust review failed: %s",
			 morph_strerror(hooks_rc));

	if (plain) {
		cli_run_plain(ctx);

		return;
	}

#ifdef HAVE_READLINE
	cli_application_run(ctx);
#else
	cli_run_plain(ctx);
#endif
}
