#include "tui/components.h"
#include "util/signals.h"
#include "util/error.h"

#include <unistd.h>

int tui_screen_open(struct tui_screen *screen, FILE *input, FILE *output, int flags)
{
	if (!screen || !input || !output)
		MORPH_RETURN(-EINVAL);
	int rc = tui_session_enter(&screen->session, input, output, flags);

	if (rc != 0)
		MORPH_RETURN(rc);
	screen->input = (struct tui_input){0};
	screen->output_fd = fileno(output);
	screen->area.x = 1;
	screen->area.y = 1;
	tui_dimensions(screen->output_fd, &screen->area.width, &screen->area.height);
	screen->pasting = 0;
	screen->dirty = 1;
	return 0;
}

int tui_screen_poll(struct tui_screen *screen, struct tui_event *event, int timeout_ms)
{
	if (!screen || !event || !screen->session.active)
		MORPH_RETURN(-EINVAL);
	*event = (struct tui_event){.type = TUI_EVENT_TICK};
	if (morph_signal_flag_get(screen->interrupted))
		MORPH_RETURN(-EINTR);
	int columns, rows;

	tui_dimensions(screen->output_fd, &columns, &rows);
	if (columns != screen->area.width || rows != screen->area.height) {
		screen->area.width = columns;
		screen->area.height = rows;
		screen->dirty = 1;
		event->type = TUI_EVENT_RESIZE;
		return 0;
	}
	int key = tui_input_read(&screen->input, screen->session.input_fd, timeout_ms);

	if (key == -ECANCELED) {
		event->type = TUI_EVENT_EOF;
		return 0;
	}
	if (key < 0)
		MORPH_RETURN(key);
	if (!key)
		return 0;
	if (key == TUI_KEY_PASTE_BEGIN)
		screen->pasting = 1;
	event->type = TUI_EVENT_KEY;
	event->key = key;
	event->pasting = screen->pasting;
	if (key == TUI_KEY_PASTE_END)
		screen->pasting = 0;
	return 0;
}

int tui_screen_run(struct tui_screen *screen, const struct tui_screen_ops *ops, void *data)
{
	if (!screen || !ops || !ops->update || !ops->draw)
		MORPH_RETURN(-EINVAL);
	for (;;) {
		if (morph_signal_flag_get(screen->interrupted))
			MORPH_RETURN(-EINTR);
		if (screen->dirty) {
			int rc = ops->draw(data, screen);

			if (rc != 0)
				MORPH_RETURN(rc);
			screen->dirty = 0;
		}
		struct tui_event event;
		int rc = tui_screen_poll(screen, &event, 100);

		if (rc == -EINTR)
			continue;
		if (rc != 0)
			MORPH_RETURN(rc);
		if (event.type == TUI_EVENT_EOF)
			return 0;
		rc = ops->update(data, &event);
		if (rc < 0)
			MORPH_RETURN(rc);
		if (rc == TUI_SCREEN_CLOSE)
			return 0;
		if (rc == TUI_SCREEN_REDRAW)
			screen->dirty = 1;
	}
}

int tui_screen_close(struct tui_screen *screen)
{
	if (!screen)
		return 0;
	return tui_session_leave(&screen->session);
}
