#include "tui/tui.h"
#include "util/error.h"

#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int tui_available(FILE *input, FILE *output)
{
	const char *term = getenv("TERM");

	return input && output && isatty(fileno(input)) &&
		isatty(fileno(output)) && (!term || strcmp(term, "dumb") != 0);
}

void tui_dimensions(int output_fd, int *columns, int *rows)
{
	struct winsize size = {0};

	*columns = 80;
	*rows = 24;
	if (ioctl(output_fd, TIOCGWINSZ, &size) == 0) {
		if (size.ws_col) *columns = size.ws_col;
		if (size.ws_row) *rows = size.ws_row;
	}
}

int tui_session_enter(struct tui_session *session, FILE *input, FILE *output,
		      int flags)
{
	struct termios raw;

	if (!session || !input || !output)
		MORPH_RETURN(-EINVAL);
	if (session->active)
		MORPH_RETURN(-EBUSY);
	int fd = fileno(input);

	if (tcgetattr(fd, &session->saved) != 0)
		MORPH_RETURN(-errno);
	raw = session->saved;
	raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
	raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(fd, TCSANOW, &raw) != 0)
		MORPH_RETURN(-errno);
	session->input_fd = fd;
	session->output = output;
	session->flags = flags;
	session->active = 1;
	if (flags & TUI_SESSION_ALTERNATE)
		fputs("\033[?1049h", output);
	fputs("\033[?25l", output);
	if (flags & TUI_SESSION_PASTE)
		fputs("\033[?2004h", output);
	if (fflush(output) != 0 || ferror(output)) {
		int rc = errno ? -errno : -EIO;

		(void)tui_session_leave(session);
		MORPH_RETURN(rc);
	}
	return 0;
}

int tui_session_leave(struct tui_session *session)
{
	int rc = 0;

	if (!session || !session->active)
		return 0;
	if (session->flags & TUI_SESSION_PASTE)
		fputs("\033[?2004l", session->output);
	if (session->flags & TUI_SESSION_ALTERNATE)
		fputs("\033[0m", session->output);
	fputs("\033[?25h", session->output);
	if (session->flags & TUI_SESSION_ALTERNATE)
		fputs("\033[?1049l", session->output);
	if (fflush(session->output) != 0 || ferror(session->output))
		MORPH_SET_ERR(rc, errno ? -errno : -EIO);
	if (tcsetattr(session->input_fd, TCSANOW, &session->saved) != 0) {
		if (rc == 0)
			MORPH_SET_ERR(rc, -errno);
	} else {
		session->active = 0;
	}
	MORPH_RETURN(rc);
}

