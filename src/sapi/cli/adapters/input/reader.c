#include "sapi/cli/adapters/input/reader.h"
#include "nb/time.h"
#include "nb/err.h"
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "sapi/cli/adapters/input/state.h"
#ifdef HAVE_READLINE
#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>
#endif
#include "sapi/cli/adapters/input/cancel.h"
#include "sapi/cli/adapters/input/callbacks.h"
#include "sapi/cli/services/command_job.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/models/jobs.h"
#include "nb/cancel.h"
#include <fcntl.h>
#include <sys/select.h>

/* Preserve stdio's buffered input while waiting in bounded cancellation slices. */
char *cli_input_fgets_until(struct cli_application *ctx, char *buffer, size_t capacity,
			    FILE *stream, int64_t *deadline)
{
	int fd = fileno(stream);
	int flags = fcntl(fd, F_GETFL);
	int error = 0;
	size_t length = 0;

	if (capacity < 2 || flags < 0) {
		errno = capacity < 2 ? EINVAL : errno;
		return NULL;
	}
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return NULL;
	while (length + 1 < capacity) {
		if (ctx &&
		    (nb_atomic_int_load(&ctx->interrupted) ||
		     nb_atomic_int_load(&ctx->terminate_requested) ||
		     nb_cancel_requested(ctx->input ? ctx->input->input_cancel_token : NULL))) {
			error = EINTR;
			break;
		}
		errno = 0;
		int ch = fgetc(stream);

		if (ch != EOF) {
			if (deadline)
				*deadline = 0;
			buffer[length++] = (char)ch;
			if (ch == '\n')
				break;
			continue;
		}
		if (feof(stream))
			break;
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
			error = errno ? errno : EIO;
			break;
		}
		clearerr(stream);
		if (deadline && *deadline > 0 && nb_time_monotonic_ms() >= *deadline) {
			error = ETIMEDOUT;
			break;
		}
		struct pollfd descriptor = {.fd = fd, .events = POLLIN};

		if (poll(&descriptor, 1, 100) < 0 && errno != EINTR) {
			error = errno;
			break;
		}
	}
	(void)fcntl(fd, F_SETFL, flags);
	buffer[length] = '\0';
	errno = error;
	return !error && length ? buffer : NULL;
}

char *cli_input_fgets(struct cli_application *ctx, char *buffer, size_t capacity, FILE *stream)
{
	return cli_input_fgets_until(ctx, buffer, capacity, stream, NULL);
}

#ifdef HAVE_READLINE
/* macOS poll reports POLLNVAL for a valid /dev/tty descriptor. Approval and
 * question prompts use that device, so wait with select for both input paths. */
static int cli_reader_wait(int fd, int milliseconds)
{
	fd_set input;
	struct timeval timeout = {
		.tv_sec = milliseconds / 1000,
		.tv_usec = (milliseconds % 1000) * 1000,
	};

	if (fd < 0 || fd >= FD_SETSIZE) {
		errno = EINVAL;
		return NB_ERR_INVAL;
	}
	FD_ZERO(&input);
	FD_SET(fd, &input);
	int rc = select(fd + 1, &input, NULL, NULL, &timeout);

	return rc < 0 ? nb_status_from_errno(errno, NULL) : rc;
}

int cli_poll_getc(struct cli_application *ctx, FILE *stream)
{
	int fd = fileno(stream);

	while (!ctx || (!nb_atomic_int_load(&ctx->interrupted) &&
			!nb_cancel_requested(ctx->input ? ctx->input->input_cancel_token : NULL))) {
		int ready = cli_reader_wait(fd, 100);

		if (ready > 0) {
			if (ctx)
				ctx->input->input_deadline_ms = 0;
			int ch = rl_getc(stream);

			int suffix = 1;
			if (ch == 0x1b) {
				do {
					suffix = cli_reader_wait(fd, 30);
				} while (suffix < 0 && errno == EINTR);
			}
			if (ch == 0x1b && suffix == 0) {
				/* Consume a lone Esc even when idle, so Readline never
				 * waits for a prefix suffix and swallows the next edit. */
				if (ctx && ctx->turn_active) {
					if (!RL_ISSTATE(RL_STATE_CALLBACK)) {
						cli_request_cancel(ctx);
						rl_done = 1;
						return EOF;
					}
					if (!cli_command_job_expedite_prompts(ctx->input_job)) {
						if (ctx->jobs &&
						    cli_jobs_model_waiting(ctx->jobs))
							runtime_interrupt_background_wait(ctx->runtime);
						else
							runtime_cancel_turn(ctx->runtime);
					}
				}
				return 0;
			}
			return ch;
		}
		if (ready < 0 && errno != EINTR)
			return EOF;
		if (ctx && ctx->input->input_deadline_ms > 0 &&
		    nb_time_monotonic_ms() >= ctx->input->input_deadline_ms) {
			ctx->input->input_timed_out = 1;
			rl_done = 1;
			return EOF;
		}
	}
	rl_done = 1;
	return EOF;
}

char *cli_reader_prompt_stream_copy(struct cli_application *ctx, const char *prompt, FILE *stream)
{
	FILE *previous_stream = rl_instream;
	int (*previous)(FILE *) = rl_getc_function;
	int (*reader)(FILE *) = ctx ? cli_callback_getc(ctx, cli_poll_getc) : rl_getc;
	int previous_catch_signals = rl_catch_signals;
	int previous_catch_winch = rl_catch_sigwinch;
	char *line;

	if (!reader)
		return NULL;
	if (stream)
		rl_instream = stream;
	rl_getc_function = reader;
	rl_catch_signals = 0;
	rl_catch_sigwinch = 0;
	line = readline(prompt);
	rl_getc_function = previous;
	rl_catch_signals = previous_catch_signals;
	rl_catch_sigwinch = previous_catch_winch;
	rl_instream = previous_stream;
	return line;
}

char *cli_readline_prompt_copy(struct cli_application *ctx, const char *prompt)
{
	return cli_reader_prompt_stream_copy(ctx, prompt, NULL);
}

void cli_reader_remember(const char *text)
{
	if (text && text[0])
		add_history(text);
}
#endif
