#include <morph/tui/screen.h>
#include "nb/signal.h"
#include "errors.h"

#include <unistd.h>

int morph_tui_screen_open(struct morph_tui_screen *screen, FILE *input, FILE *output, int flags)
{
	if (!screen || !input || !output)
		return NB_ERR_INVAL;
	int rc = morph_tui_terminal_session_enter(&screen->session, input, output, flags);

	if (rc != 0)
		return rc;
	screen->input = (struct morph_tui_input){0};
	screen->output_fd = fileno(output);
	screen->area.x = 1;
	screen->area.y = 1;
	morph_tui_terminal_dimensions(screen->output_fd, &screen->area.width, &screen->area.height);
	screen->pasting = 0;
	screen->dirty = 1;
	return 0;
}

int morph_tui_screen_poll(struct morph_tui_screen *screen, struct morph_tui_event *event,
			  int timeout_ms)
{
	if (!screen || !event || !screen->session.active)
		return NB_ERR_INVAL;
	*event = (struct morph_tui_event){.type = MORPH_TUI_EVENT_TICK};
	if (nb_atomic_int_load(screen->interrupted))
		return NB_ERR_INTERRUPTED;
	int columns, rows;

	morph_tui_terminal_dimensions(screen->output_fd, &columns, &rows);
	if (columns != screen->area.width || rows != screen->area.height) {
		screen->area.width = columns;
		screen->area.height = rows;
		screen->dirty = 1;
		event->type = MORPH_TUI_EVENT_RESIZE;
		return 0;
	}
	int key = morph_tui_input_read(&screen->input, screen->session.input_fd, timeout_ms);

	if (key == NB_ERR_CANCELLED) {
		event->type = MORPH_TUI_EVENT_EOF;
		return 0;
	}
	if (key < 0)
		return key;
	if (!key)
		return 0;
	if (key == MORPH_TUI_KEY_PASTE_BEGIN)
		screen->pasting = 1;
	event->type = MORPH_TUI_EVENT_KEY;
	event->key = key;
	event->pasting = screen->pasting;
	if (key == MORPH_TUI_KEY_MOUSE) {
		event->type = MORPH_TUI_EVENT_MOUSE;
		event->mouse = screen->input.mouse;
	}
	if (key == MORPH_TUI_KEY_PASTE_END)
		screen->pasting = 0;
	return 0;
}

int morph_tui_screen_run(struct morph_tui_screen *screen, const struct morph_tui_screen_ops *ops,
			 void *data)
{
	if (!screen || !ops || !ops->update || !ops->draw)
		return NB_ERR_INVAL;
	for (;;) {
		if (nb_atomic_int_load(screen->interrupted))
			return NB_ERR_INTERRUPTED;
		if (screen->dirty) {
			int rc = ops->draw(data, screen);

			if (rc != 0)
				return rc;
			screen->dirty = 0;
		}
		struct morph_tui_event event;
		int rc = morph_tui_screen_poll(screen, &event, 100);

		if (rc == NB_ERR_INTERRUPTED)
			continue;
		if (rc != 0)
			return rc;
		if (event.type == MORPH_TUI_EVENT_EOF)
			return 0;
		rc = ops->update(data, &event);
		if (rc < 0)
			return rc;
		if (rc == MORPH_TUI_SCREEN_CLOSE)
			return 0;
		if (rc == MORPH_TUI_SCREEN_REDRAW)
			screen->dirty = 1;
	}
}

int morph_tui_screen_close(struct morph_tui_screen *screen)
{
	if (!screen)
		return 0;
	return morph_tui_terminal_session_leave(&screen->session);
}
