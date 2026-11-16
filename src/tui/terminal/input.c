#include <morph/tui/input.h>
#include "errors.h"
#include <nb/time.h>

#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TERMINAL_INPUT_ESCAPE_TIMEOUT_MS 80

enum terminal_input_state {
	TERMINAL_INPUT_TEXT,
	TERMINAL_INPUT_ESCAPE,
	TERMINAL_INPUT_CSI,
	INPUT_SS3,
	TERMINAL_INPUT_DISCARD
};

static int terminal_input_mouse(struct morph_tui_input *input, const char *sequence)
{
	unsigned button, column, row;
	char action;
	int consumed = 0;

	if (sscanf(sequence, "<%u;%u;%u%c%n", &button, &column, &row, &action, &consumed) != 4 ||
	    sequence[consumed] || (action != 'M' && action != 'm') ||
	    !column || !row || column > INT_MAX || row > INT_MAX || button > 127U)
		return 0;
	if (button & 64U)
		return action == 'M' && (button & 3U) < 2U ?
			(button & 1U ? MORPH_TUI_KEY_WHEEL_DOWN : MORPH_TUI_KEY_WHEEL_UP) : 0;
	if ((button & 3U) == 3U)
		return 0;
	input->mouse = (struct morph_tui_mouse_event){
		.button = (enum morph_tui_mouse_button)(button & 3U),
		.action = action == 'm' ? MORPH_TUI_MOUSE_RELEASE :
			(button & 32U ? MORPH_TUI_MOUSE_MOVE : MORPH_TUI_MOUSE_PRESS),
		.x = (int)column - 1,
		.y = (int)row - 1,
		.modifiers = (button >> 2U) & 7U,
	};
	return MORPH_TUI_KEY_MOUSE;
}

static int terminal_input_shortcut(unsigned code, unsigned modifiers)
{
	/* Lock keys do not change shortcut matching. Protocol modifiers are one-based. */
	if (!modifiers || modifiers > 256U)
		return 0;
	unsigned mods = (modifiers - 1U) & ~192U;
	unsigned letter = code >= 'A' && code <= 'Z' ? code + ('a' - 'A') : code;

	if (letter == 'v' && mods == 12U)
		return MORPH_TUI_KEY_PASTE_IMAGE;
	if (mods == 8U || mods == 5U) {
		if (letter == 'c')
			return MORPH_TUI_KEY_COPY;
		if (letter == 'v')
			return MORPH_TUI_KEY_PASTE_CLIPBOARD;
		if (letter == 'z' && mods == 8U)
			return 31;
	}
	if (code == 13U && (mods == 1U || mods == 2U))
		return '\n';
	if (code == 9U && mods == 1U)
		return MORPH_TUI_KEY_BACK_TAB;
	if (mods == 4U) {
		if (letter >= 'a' && letter <= 'z')
			return (int)(letter - 'a' + 1U);
		if (code >= '@' && code <= '_')
			return (int)(code - '@');
	}
	return mods == 0U && code < 128U ? (int)code : 0;
}

static int terminal_input_number(const char **sequence, unsigned *number)
{
	const char *p = *sequence;
	unsigned value = 0;
	if (*p < '0' || *p > '9')
		return 0;
	while (*p >= '0' && *p <= '9') {
		unsigned digit = (unsigned)(*p++ - '0');
		if (value > (UINT_MAX - digit) / 10U)
			return 0;
		value = value * 10U + digit;
	}
	*sequence = p;
	*number = value;
	return 1;
}

static int terminal_input_extended_key(const char *sequence)
{
	unsigned code = 0, modifiers = 1, type = 1;
	const char *p = sequence;
	if (!terminal_input_number(&p, &code))
		return 0;
	/* xterm modifyOtherKeys: CSI 27 ; modifiers ; code ~. */
	if (code == 27U && *p == ';' && strchr(p, '~')) {
		p++;
		if (!terminal_input_number(&p, &modifiers) || *p++ != ';' ||
		    !terminal_input_number(&p, &code) || strcmp(p, "~"))
			return 0;
		return terminal_input_shortcut(code, modifiers);
	}
	/* CSI u may include alternate key codes and an explicit press/repeat type. */
	while (*p == ':') {
		unsigned alternate = 0;
		p++;
		if (!terminal_input_number(&p, &alternate))
			return 0;
	}
	if (*p == ';') {
		p++;
		if (!terminal_input_number(&p, &modifiers))
			return 0;
		if (*p == ':') {
			p++;
			if (!terminal_input_number(&p, &type))
				return 0;
		}
	}
	if (strcmp(p, "u") || (type != 1U && type != 2U))
		return 0;
	return terminal_input_shortcut(code, modifiers);
}

static int terminal_input_sequence_key(struct morph_tui_input *input, const char *sequence)
{
	if (strcmp(sequence, "A") == 0)
		return MORPH_TUI_KEY_UP;
	if (strcmp(sequence, "B") == 0)
		return MORPH_TUI_KEY_DOWN;
	if (strcmp(sequence, "Z") == 0)
		return MORPH_TUI_KEY_BACK_TAB;
	if (strcmp(sequence, "C") == 0)
		return MORPH_TUI_KEY_RIGHT;
	if (strcmp(sequence, "D") == 0)
		return MORPH_TUI_KEY_LEFT;
	if (strcmp(sequence, "H") == 0 || strcmp(sequence, "1~") == 0 ||
	    strcmp(sequence, "7~") == 0)
		return MORPH_TUI_KEY_HOME;
	if (strcmp(sequence, "F") == 0 || strcmp(sequence, "4~") == 0 ||
	    strcmp(sequence, "8~") == 0)
		return MORPH_TUI_KEY_END;
	if (strcmp(sequence, "5~") == 0)
		return MORPH_TUI_KEY_PAGE_UP;
	if (strcmp(sequence, "6~") == 0)
		return MORPH_TUI_KEY_PAGE_DOWN;
	if (strcmp(sequence, "3~") == 0)
		return MORPH_TUI_KEY_DELETE;
	if (strcmp(sequence, "2;5~") == 0)
		return MORPH_TUI_KEY_COPY;
	if (strcmp(sequence, "2;2~") == 0)
		return MORPH_TUI_KEY_PASTE_CLIPBOARD;
	if (strcmp(sequence, "200~") == 0)
		return MORPH_TUI_KEY_PASTE_BEGIN;
	if (strcmp(sequence, "201~") == 0)
		return MORPH_TUI_KEY_PASTE_END;
	if (strcmp(sequence, "I") == 0)
		return MORPH_TUI_KEY_FOCUS_IN;
	if (strcmp(sequence, "O") == 0)
		return MORPH_TUI_KEY_FOCUS_OUT;
	if (sequence[0] >= '0' && sequence[0] <= '9')
		return terminal_input_extended_key(sequence);
	return terminal_input_mouse(input, sequence);
}

int morph_tui_input_feed(struct morph_tui_input *input, unsigned char byte)
{
	if (!input)
		return NB_ERR_INVAL;
	input->deadline_ms = 0;
	if (byte == 27) {
		input->state = TERMINAL_INPUT_ESCAPE;
		input->length = 0;
		return 0;
	}
	if (input->state == TERMINAL_INPUT_TEXT)
		return byte;
	if (input->state == TERMINAL_INPUT_ESCAPE) {
		if (byte == '\r') {
			input->state = TERMINAL_INPUT_TEXT;
			return '\n';
		}
		input->state = byte == '['   ? TERMINAL_INPUT_CSI
			       : byte == 'O' ? INPUT_SS3
					     : TERMINAL_INPUT_TEXT;
		return 0;
	}
	if (byte < 0x20 || byte > 0x7e) {
		input->state = TERMINAL_INPUT_TEXT;
		input->length = 0;
		return 0;
	}
	if (input->length + 1 >= sizeof(input->sequence))
		input->state = TERMINAL_INPUT_DISCARD;
	if (input->state != TERMINAL_INPUT_DISCARD)
		input->sequence[input->length++] = (char)byte;
	if (byte < '@')
		return 0;
	int key = 0;

	if (input->state != TERMINAL_INPUT_DISCARD) {
		input->sequence[input->length] = '\0';
		key = terminal_input_sequence_key(input, input->sequence);
	}
	input->length = 0;
	input->state = TERMINAL_INPUT_TEXT;
	return key;
}

int morph_tui_input_expire(struct morph_tui_input *input)
{
	if (!input)
		return NB_ERR_INVAL;
	int key = input->state == TERMINAL_INPUT_ESCAPE ? 27 : 0;

	input->state = TERMINAL_INPUT_TEXT;
	input->length = 0;
	input->deadline_ms = 0;
	return key;
}

static int terminal_input_monotonic_ms(int64_t *value)
{
	*value = nb_time_monotonic_ms();
	return *value > 0 ? 0 : NB_ERR_IO;
}

static int terminal_input_remaining_ms(int64_t deadline, int64_t now)
{
	int64_t remaining = deadline - now;

	return remaining <= 0 ? 0 : remaining > INT_MAX ? INT_MAX : (int)remaining;
}

int morph_tui_input_read(struct morph_tui_input *input, int input_fd, int timeout_ms)
{
	int64_t now;

	if (!input)
		return NB_ERR_INVAL;
	if (input_fd < 0)
		return NB_ERR_BAD_HANDLE;
	int rc = terminal_input_monotonic_ms(&now);

	if (rc != 0)
		return rc;
	int64_t deadline = timeout_ms < 0 ? INT64_MAX : now + timeout_ms;

	for (;;) {
		int wait = timeout_ms < 0 ? -1 : terminal_input_remaining_ms(deadline, now);

		if (input->state != TERMINAL_INPUT_TEXT) {
			if (!input->deadline_ms)
				input->deadline_ms = now + TERMINAL_INPUT_ESCAPE_TIMEOUT_MS;
			if (now >= input->deadline_ms)
				return morph_tui_input_expire(input);
			int escape_wait = terminal_input_remaining_ms(input->deadline_ms, now);

			if (wait < 0 || escape_wait < wait)
				wait = escape_wait;
		}
		struct pollfd fd = {.fd = input_fd, .events = POLLIN};
		int ready = poll(&fd, 1, wait);

		if (ready < 0)
			return nb_status_from_errno(errno, NULL);
		if (fd.revents & POLLNVAL)
			return NB_ERR_BAD_HANDLE;
		if (ready) {
			unsigned char byte;
			ssize_t count = read(input_fd, &byte, sizeof(byte));

			if (count < 0)
				return nb_status_from_errno(errno, NULL);
			if (!count) {
				int key = morph_tui_input_expire(input);

				return key ? key : NB_ERR_CANCELLED;
			}
			int key = morph_tui_input_feed(input, byte);

			if (key || input->state == TERMINAL_INPUT_TEXT)
				return key;
		}
		rc = terminal_input_monotonic_ms(&now);
		if (rc != 0)
			return rc;
		if (ready)
			input->deadline_ms = now + TERMINAL_INPUT_ESCAPE_TIMEOUT_MS;
		if (!ready && input->state != TERMINAL_INPUT_TEXT && now >= input->deadline_ms)
			return morph_tui_input_expire(input);
		if ((!ready || timeout_ms > 0) && now >= deadline)
			return 0;
	}
}
