#include <morph/tui.h>
#include "util/error.h"

#include <limits.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MORPH_TUI_ESCAPE_TIMEOUT_MS 80

enum input_state {
	INPUT_TEXT,
	INPUT_ESCAPE,
	INPUT_CSI,
	INPUT_SS3,
	INPUT_DISCARD
};

static int sequence_key(const char *sequence)
{
	if (strcmp(sequence, "A") == 0) return MORPH_TUI_KEY_UP;
	if (strcmp(sequence, "B") == 0) return MORPH_TUI_KEY_DOWN;
	if (strcmp(sequence, "C") == 0) return MORPH_TUI_KEY_RIGHT;
	if (strcmp(sequence, "D") == 0) return MORPH_TUI_KEY_LEFT;
	if (strcmp(sequence, "H") == 0 || strcmp(sequence, "1~") == 0 ||
	    strcmp(sequence, "7~") == 0)
		return MORPH_TUI_KEY_HOME;
	if (strcmp(sequence, "F") == 0 || strcmp(sequence, "4~") == 0 ||
	    strcmp(sequence, "8~") == 0)
		return MORPH_TUI_KEY_END;
	if (strcmp(sequence, "5~") == 0) return MORPH_TUI_KEY_PAGE_UP;
	if (strcmp(sequence, "6~") == 0) return MORPH_TUI_KEY_PAGE_DOWN;
	if (strcmp(sequence, "200~") == 0) return MORPH_TUI_KEY_PASTE_BEGIN;
	if (strcmp(sequence, "201~") == 0) return MORPH_TUI_KEY_PASTE_END;
	return 0;
}

int morph_tui_input_feed(struct morph_tui_input *input, unsigned char byte)
{
	if (!input)
		MORPH_RETURN(-EINVAL);
	input->deadline_ms = 0;
	if (byte == 27) {
		input->state = INPUT_ESCAPE;
		input->length = 0;
		return 0;
	}
	if (input->state == INPUT_TEXT)
		return byte;
	if (input->state == INPUT_ESCAPE) {
		input->state = byte == '[' ? INPUT_CSI :
			byte == 'O' ? INPUT_SS3 : INPUT_TEXT;
		return 0;
	}
	if (byte < 0x20 || byte > 0x7e) {
		input->state = INPUT_TEXT;
		input->length = 0;
		return 0;
	}
	if (input->length + 1 >= sizeof(input->sequence))
		input->state = INPUT_DISCARD;
	if (input->state != INPUT_DISCARD)
		input->sequence[input->length++] = (char)byte;
	if (byte < '@')
		return 0;
	int key = 0;

	if (input->state != INPUT_DISCARD) {
		input->sequence[input->length] = '\0';
		key = sequence_key(input->sequence);
	}
	input->length = 0;
	input->state = INPUT_TEXT;
	return key;
}

int morph_tui_input_expire(struct morph_tui_input *input)
{
	if (!input)
		MORPH_RETURN(-EINVAL);
	int key = input->state == INPUT_ESCAPE ? 27 : 0;

	input->state = INPUT_TEXT;
	input->length = 0;
	input->deadline_ms = 0;
	return key;
}

static int monotonic_ms(int64_t *value)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		MORPH_RETURN(-errno);
	*value = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
	return 0;
}

static int remaining_ms(int64_t deadline, int64_t now)
{
	int64_t remaining = deadline - now;

	return remaining <= 0 ? 0 : remaining > INT_MAX ? INT_MAX : (int)remaining;
}

int morph_tui_input_read(struct morph_tui_input *input, int input_fd, int timeout_ms)
{
	int64_t now;

	if (!input)
		MORPH_RETURN(-EINVAL);
	if (input_fd < 0)
		MORPH_RETURN(-EBADF);
	int rc = monotonic_ms(&now);

	if (rc != 0)
		MORPH_RETURN(rc);
	int64_t deadline = timeout_ms < 0 ? INT64_MAX : now + timeout_ms;

	for (;;) {
		int wait = timeout_ms < 0 ? -1 : remaining_ms(deadline, now);

		if (input->state != INPUT_TEXT) {
			if (!input->deadline_ms)
				input->deadline_ms = now + MORPH_TUI_ESCAPE_TIMEOUT_MS;
			if (now >= input->deadline_ms)
				return morph_tui_input_expire(input);
			int escape_wait = remaining_ms(input->deadline_ms, now);

			if (wait < 0 || escape_wait < wait)
				wait = escape_wait;
		}
		struct pollfd fd = {.fd = input_fd, .events = POLLIN};
		int ready = poll(&fd, 1, wait);

		if (ready < 0)
			MORPH_RETURN(-errno);
		if (fd.revents & POLLNVAL)
			MORPH_RETURN(-EBADF);
		if (ready) {
			unsigned char byte;
			ssize_t count = read(input_fd, &byte, sizeof(byte));

			if (count < 0)
				MORPH_RETURN(-errno);
			if (!count) {
				int key = morph_tui_input_expire(input);

				return key ? key : -ECANCELED;
			}
			int key = morph_tui_input_feed(input, byte);

			if (key || input->state == INPUT_TEXT)
				return key;
		}
		rc = monotonic_ms(&now);
		if (rc != 0)
			MORPH_RETURN(rc);
		if (ready)
			input->deadline_ms = now + MORPH_TUI_ESCAPE_TIMEOUT_MS;
		if (!ready && input->state != INPUT_TEXT && now >= input->deadline_ms)
			return morph_tui_input_expire(input);
		if ((!ready || timeout_ms > 0) && now >= deadline)
			return 0;
	}
}
