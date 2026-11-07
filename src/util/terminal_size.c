#include "terminal_size.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/ioctl.h>

int morph_terminal_columns(int output_fd, int fallback)
{
	struct winsize size = {0};
	const char *configured;
	char *end;
	long columns;

	if (output_fd >= 0 && ioctl(output_fd, TIOCGWINSZ, &size) == 0 && size.ws_col)
		return size.ws_col;
	configured = getenv("COLUMNS");
	if (!configured || !*configured)
		return fallback;
	errno = 0;
	columns = strtol(configured, &end, 10);
	if (errno || *end || columns <= 0 || columns > INT_MAX)
		return fallback;
	return (int)columns;
}

void morph_terminal_dimensions(int output_fd, int *columns, int *rows)
{
	struct winsize size = {0};

	*columns = morph_terminal_columns(output_fd, 80);
	*rows = 24;
	if (ioctl(output_fd, TIOCGWINSZ, &size) == 0 && size.ws_row)
		*rows = size.ws_row;
}
