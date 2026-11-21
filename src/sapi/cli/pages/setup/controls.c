#include "sapi/cli/pages/setup/controls.h"

#include "sapi/cli/application.h"
#include "sapi/cli/components/setup_form.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/services/configuration/model.h"
#include "sapi/cli/services/configuration/role.h"
#include "terminal/text.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include <nb/thread.h>
#include <nothing/tui/draw.h>
#include <nothing/tui/screen.h>
#include <nothing/tui/text.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct cli_controls_setup_ui {
	nb_atomic_int interrupted;
	nb_signal_listener *signals;
	struct nt_screen screen;
	FILE *output;
	int fd;
	int output_fd;
	int color;
	int columns;
	int rows;
	struct nt_inline_region region;
};

int cli_setup_controls_available(FILE *input, FILE *output)
{
	return nt_terminal_available(input, output);
}

int cli_setup_print(FILE *output, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	if (!isatty(fileno(output))) {
		int written = vfprintf(output, format, args);
		va_end(args);
		return written < 0 ? NB_ERR_IO : 0;
	}

	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf rendered NB_CLEANUP(nb_buf_cleanup) = {0};
	struct nt_text_layout view;
	nt_text_layout_init(&view);
	int rc = (nb_buf_init(&text), nb_buf_reserve(&text, 4096) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_vprintf(&text, format, args) ? 0 : NB_ERR_NOMEM);

	va_end(args);
	char *safe = rc == 0 ? morph_terminal_sanitize_dup(nb_buf_cstr(&text), text.len,
							   MORPH_TERMINAL_TEXT_MULTILINE, NULL)
			     : NULL;
	if (rc == 0 && !safe)
		rc = NB_ERR_NOMEM;

	int columns, rows;
	nt_terminal_dimensions(fileno(output), &columns, &rows);
	size_t width = columns > 4 ? (size_t)columns - 4 : 1;
	if (rc == 0)
		rc = nt_text_layout_build(&view, safe, width, NT_TEXT_WRAP_CHAR);

	if (rc == 0)
		rc = (nb_buf_init(&rendered), nb_buf_reserve(&rendered, 4096) ? 0 : NB_ERR_NOMEM);

	for (size_t i = 0; rc == 0 && i < view.lines.len; i++) {
		const struct nt_text_line *row = nb_vec_at(&view.lines, i);
		if (row->length)
			rc = (nb_buf_append_cstr(&rendered, "  ") ? 0 : NB_ERR_NOMEM);

		if (rc == 0)
			rc = (nb_buf_append(&rendered, safe + row->offset, row->length)
				      ? 0
				      : NB_ERR_NOMEM);

		if (rc == 0)
			rc = (nb_buf_append_cstr(&rendered, "\r\n") ? 0 : NB_ERR_NOMEM);
	}

	if (rc == 0)
		rc = nt_draw_write(output, &rendered);

	nt_text_layout_deinit(&view);
	nb_buf_cleanup(&rendered);
	nb_buf_cleanup(&text);
	free(safe);
	return rc;
}

static void cli_controls_setup_signal(int number, void *opaque)
{
	struct cli_controls_setup_ui *ui = opaque;

	if (number != SIGWINCH)
		nb_atomic_int_store(&ui->interrupted, number);
}

static void cli_controls_dimensions(struct cli_controls_setup_ui *ui)
{
	nt_terminal_dimensions(ui->output_fd, &ui->columns, &ui->rows);
}

static int cli_controls_read_event(struct cli_controls_setup_ui *ui, struct nt_event *event)
{
	for (;;) {
		int rc = nt_screen_poll(&ui->screen, event, 100);

		if (nb_atomic_int_load(&ui->interrupted))
			return NB_ERR_CANCELLED;

		if (rc == NB_ERR_INTERRUPTED)
			continue;

		if (rc != 0)
			return rc;

		if (event->type == NT_EVENT_EOF)
			return NB_ERR_CANCELLED;

		if (event->type == NT_EVENT_RESIZE || event->type == NT_EVENT_KEY ||
		    event->type == NT_EVENT_TEXT || event->type == NT_EVENT_PASTE)
			return 1;
	}
}

int cli_setup_controls_collect(struct cli_application *ctx, const char *path, FILE *input,
			       FILE *output, struct cli_setup_state *state)
{
	cli_setup_role_defaults(state);
	if (!state->existing_prompt)
		state->replace_prompt = 1;

	struct cli_controls_setup_ui ui = {
		.output = output,
		.fd = fileno(input),
		.output_fd = fileno(output),
		.color = cli_output_color_enabled(ctx) && !getenv("NO_COLOR"),
	};
	int rc = nb_signal_listener_start(
		&ui.signals, (const int[]){SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGWINCH}, 5,
		cli_controls_setup_signal, &ui, NULL);
	if (rc != 0)
		return rc;

	rc = nt_screen_open(&ui.screen, input, output,
			    NT_TERMINAL_PASTE | (ui.color ? 0 : NT_TERMINAL_NO_SGR));
	ui.screen.interrupted = &ui.interrupted;
	if (rc != 0) {
		nb_signal_listener_stop(&ui.signals, NULL);
		return rc;
	}

	cli_controls_dimensions(&ui);
	struct cli_setup_form *form = NULL;
	if (rc == 0)
		rc = cli_setup_form_create(state, path, ui.color, ui.columns, &form);

	while (rc == 0) {
		rc = cli_setup_form_show(form, &ui.region, output, ui.columns, ui.rows);
		if (rc != 0)
			break;

		struct nt_event event;
		rc = cli_controls_read_event(&ui, &event);
		if (rc < 0)
			break;

		if (event.type == NT_EVENT_RESIZE) {
			cli_controls_dimensions(&ui);
			rc = 0;
			continue;
		}

		int result = cli_setup_form_dispatch(form, &event);
		if (result == CLI_SETUP_FORM_SUBMIT) {
			rc = 0;
			break;
		}

		if (result == CLI_SETUP_FORM_CANCEL) {
			rc = NB_ERR_CANCELLED;
			break;
		}

		if (result < 0) {
			rc = result;
			break;
		}

		rc = 0;
	}

	cli_controls_dimensions(&ui);
	int ending_rc = nt_inline_region_commit(&ui.region, output, NULL, ui.columns, ui.rows);
	if (rc == 0 && ending_rc < 0)
		rc = ending_rc;

	cli_setup_form_destroy(&form);
	int leave_rc = nt_screen_close(&ui.screen);
	if (rc == 0 && leave_rc < 0)
		rc = leave_rc;

	if (rc == 0 && fputc('\r', output) == EOF)
		rc = NB_ERR_IO;

	nb_signal_listener_stop(&ui.signals, NULL);
	return rc;
}
