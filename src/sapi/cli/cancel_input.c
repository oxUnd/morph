#include "sapi/cli/internal.h"
#include "http/client.h"

#include <sys/select.h>
#include <termios.h>

#define CLI_ESCAPE_SEQUENCE_WAIT_US 30000

struct cli_cancel_monitor {
	pthread_t thread;
	volatile sig_atomic_t stop;
	int fd;
	struct termios saved_termios;
};

static int escape_has_sequence_suffix(int fd)
{
	unsigned char discard[16];
	struct timeval timeout;
	fd_set read_fds;
	int rc;

	FD_ZERO(&read_fds);
	FD_SET(fd, &read_fds);
	timeout.tv_sec = 0;
	timeout.tv_usec = CLI_ESCAPE_SEQUENCE_WAIT_US;
	do {
		rc = select(fd + 1, &read_fds, NULL, NULL, &timeout);
	} while (rc < 0 && errno == EINTR);
	if (rc <= 0)
		return 0;
	(void)read(fd, discard, sizeof(discard));
	return 1;
}

static void *cancel_monitor_thread(void *user_data)
{
	struct cli_cancel_monitor *monitor = user_data;

	while (!monitor->stop) {
		unsigned char ch;
		ssize_t count = read(monitor->fd, &ch, sizeof(ch));

		if (count < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (count == 0 || ch != 0x1b)
			continue;
		if (escape_has_sequence_suffix(monitor->fd) || monitor->stop)
			continue;
		react_cancel_active();
		http_cancel_from_signal();
		cli_sigint_received = 1;
		break;
	}
	return NULL;
}

void cli_cancel_state_reset(void)
{
	react_sigint_flag = 0;
	http_clear_signal_cancel();
	cli_sigint_received = 0;
}

struct cli_cancel_monitor *cli_cancel_monitor_start(int fd)
{
	struct cli_cancel_monitor *monitor;
	struct termios active_termios;

	if (fd < 0 || !isatty(fd))
		return NULL;
	monitor = calloc(1, sizeof(*monitor));
	if (!monitor)
		return NULL;
	monitor->fd = fd;
	if (tcgetattr(fd, &monitor->saved_termios) != 0) {
		free(monitor);
		return NULL;
	}
	active_termios = monitor->saved_termios;
	active_termios.c_lflag &= (tcflag_t)~(ICANON | ECHO);
	active_termios.c_cc[VMIN] = 0;
	active_termios.c_cc[VTIME] = 1;
	if (tcsetattr(fd, TCSANOW, &active_termios) != 0) {
		free(monitor);
		return NULL;
	}
	if (pthread_create(&monitor->thread, NULL, cancel_monitor_thread,
			   monitor) != 0) {
		(void)tcsetattr(fd, TCSANOW, &monitor->saved_termios);
		free(monitor);
		return NULL;
	}
	return monitor;
}

void cli_cancel_monitor_stop(struct cli_cancel_monitor *monitor)
{
	if (!monitor)
		return;
	monitor->stop = 1;
	(void)pthread_join(monitor->thread, NULL);
	(void)tcsetattr(monitor->fd, TCSANOW, &monitor->saved_termios);
	free(monitor);
}
