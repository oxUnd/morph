#include <morph/tui/terminal.h>
#include "util/error.h"
#include "util/terminal_size.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int morph_tui_terminal_available(FILE *input, FILE *output)
{
	const char *term = getenv("TERM");

	return input && output && isatty(fileno(input)) && isatty(fileno(output)) &&
	       (!term || strcmp(term, "dumb") != 0);
}

void morph_tui_terminal_dimensions(int output_fd, int *columns, int *rows)
{
	morph_terminal_dimensions(output_fd, columns, rows);
}

int morph_tui_terminal_session_enter(struct morph_tui_terminal_session *session, FILE *input,
				     FILE *output, int flags)
{
	struct termios raw;

	if (!session || !input || !output)
		return -EINVAL;
	if (session->active)
		return -EBUSY;
	int fd = fileno(input);

	if (tcgetattr(fd, &session->saved) != 0)
		return -errno;
	raw = session->saved;
	raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO | ISIG | IEXTEN);
	raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(fd, TCSANOW, &raw) != 0)
		return -errno;
	session->input_fd = fd;
	session->output = output;
	session->flags = flags;
	session->active = 1;
	if (flags & MORPH_TUI_TERMINAL_SESSION_ALTERNATE)
		fputs("\033[?1049h", output);
	fputs("\033[?25l", output);
	if (flags & MORPH_TUI_TERMINAL_SESSION_PASTE)
		fputs("\033[?2004h", output);
	if (flags & MORPH_TUI_TERMINAL_SESSION_FOCUS)
		fputs("\033[?1004h", output);
	if (flags & MORPH_TUI_TERMINAL_SESSION_MOUSE)
		fputs("\033[?1000h\033[?1006h", output);
	if (fflush(output) != 0 || ferror(output)) {
		int rc = errno ? -errno : -EIO;

		(void)morph_tui_terminal_session_leave(session);
		return rc;
	}
	return 0;
}

int morph_tui_terminal_session_leave(struct morph_tui_terminal_session *session)
{
	int rc = 0;

	if (!session || !session->active)
		return 0;
	if (session->flags & MORPH_TUI_TERMINAL_SESSION_MOUSE)
		fputs("\033[?1006l\033[?1000l", session->output);
	if (session->flags & MORPH_TUI_TERMINAL_SESSION_PASTE)
		fputs("\033[?2004l", session->output);
	if (session->flags & MORPH_TUI_TERMINAL_SESSION_FOCUS)
		fputs("\033[?1004l", session->output);
	if (session->flags & MORPH_TUI_TERMINAL_SESSION_ALTERNATE)
		fputs("\033[0m", session->output);
	fputs("\033[?25h", session->output);
	if (session->flags & MORPH_TUI_TERMINAL_SESSION_ALTERNATE)
		fputs("\033[?1049l", session->output);
	if (fflush(session->output) != 0 || ferror(session->output))
		rc = (errno ? -errno : -EIO);
	if (tcsetattr(session->input_fd, TCSANOW, &session->saved) != 0) {
		if (rc == 0)
			rc = (-errno);
	} else {
		session->active = 0;
	}
	return rc;
}

int morph_tui_terminal_session_reassert(struct morph_tui_terminal_session *session)
{
	if (!session || !session->active)
		return 0;
	struct termios raw = session->saved;
	raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO | ISIG | IEXTEN);
	raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(session->input_fd, TCSANOW, &raw) != 0)
		return -errno;
	if (session->flags & MORPH_TUI_TERMINAL_SESSION_PASTE)
		fputs("\033[?2004h", session->output);
	if (session->flags & MORPH_TUI_TERMINAL_SESSION_FOCUS)
		fputs("\033[?1004h", session->output);
	if (session->flags & MORPH_TUI_TERMINAL_SESSION_MOUSE)
		fputs("\033[?1000h\033[?1006h", session->output);
	return fflush(session->output) != 0 || ferror(session->output) ? (errno ? -errno : -EIO) : 0;
}
