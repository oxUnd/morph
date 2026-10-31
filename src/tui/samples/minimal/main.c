#include <morph/tui/core/application.h>
#include <morph/tui/render/draw.h>
#include <morph/tui/widgets/list.h>
#include "util/error.h"
#include "util/signals.h"

#include <stdio.h>
#include <stdlib.h>

struct sample_minimal {
	struct morph_tui_widget_menu menu;
	morph_buf_t commands;
	morph_buf_t line;
	volatile sig_atomic_t interrupted;
	const char *items[3];
	const char *selected;
};

static void sample_minimal_signal(int number, void *data)
{
	struct sample_minimal *sample = data;

	if (number != SIGWINCH)
		morph_signal_flag_set(&sample->interrupted, 1);
}

static int sample_minimal_event(void *data, struct morph_tui_core_app *app,
				const struct morph_tui_core_event *event)
{
	struct sample_minimal *sample = data;

	if (morph_signal_flag_get(&sample->interrupted))
		return morph_tui_core_app_dismiss(app, 0);
	if (event->type != MORPH_TUI_CORE_EVENT_KEY)
		return 0;
	if (!event->pasting && event->key == 'q')
		return morph_tui_core_app_dismiss(app, 0);
	int result = morph_tui_widget_menu_update(&sample->menu, event->key);

	if (result == MORPH_TUI_WIDGET_BACK || result == MORPH_TUI_WIDGET_CANCEL)
		return morph_tui_core_app_dismiss(app, 0);
	if (result == MORPH_TUI_WIDGET_SUBMIT)
		sample->selected = sample->items[sample->menu.list.index];
	if (result == MORPH_TUI_WIDGET_CHANGED || result == MORPH_TUI_WIDGET_SUBMIT)
		morph_tui_core_app_invalidate(app, MORPH_TUI_CORE_INVALIDATE_DISPLAY);
	return 1;
}

static int sample_minimal_layout(void *data, const struct morph_tui_core_layout_rect *area)
{
	struct sample_minimal *sample = data;
	size_t count = sizeof(sample->items) / sizeof(sample->items[0]);
	size_t page = area->height > 5 ? (size_t)(area->height - 5) : 1;

	morph_tui_widget_list_layout(&sample->menu.list, count, page < count ? page : count);
	return 0;
}

static int sample_minimal_draw(void *data, const struct morph_tui_core_screen *screen)
{
	struct sample_minimal *sample = data;
	const struct morph_tui_widget_list *list = &sample->menu.list;

	morph_buf_reset(&sample->commands);
	for (int row = 1; row <= screen->area.height; row++) {
		morph_buf_reset(&sample->line);
		if (row == 1)
			morph_buf_puts(&sample->line, "morph-tui minimal");
		else if (row == 2)
			morph_buf_puts(&sample->line,
				       "Up/Down or j/k: move; Enter: select; q/Esc: quit");
		else if (row == screen->area.height)
			morph_buf_printf(&sample->line, "Selected: %s",
					 sample->selected ? sample->selected : "none");
		else if (row >= 4 && (size_t)(row - 4) < list->page) {
			size_t index = list->first + (size_t)(row - 4);

			morph_buf_printf(&sample->line, "%s %s", index == list->index ? ">" : " ",
					 sample->items[index]);
		}
		if (sample->line.failed)
			return sample->line.failed;
		int rc = morph_tui_render_draw_cell_append(
			&sample->commands, screen->area.width, screen->area.height, row, 1,
			screen->area.width, morph_buf_cstr(&sample->line), "");

		if (rc)
			return rc;
	}
	return morph_tui_render_draw_write(screen->session.output, &sample->commands);
}

int main(void)
{
	struct sample_minimal sample = {.items = {"Alpha", "Beta", "Gamma"}};
	struct morph_tui_core_app *app = NULL;
	struct morph_signal_listener signals = {0};
	const struct morph_tui_core_app_options options = {.input = stdin, .output = stdout};
	const struct morph_tui_core_controller_ops ops = {
		.event = sample_minimal_event,
		.layout = sample_minimal_layout,
		.draw = sample_minimal_draw,
	};
	const struct morph_tui_core_controller controller = {
		.ops = &ops,
		.data = &sample,
		.input_mode = MORPH_TUI_CORE_INPUT_KEYS,
		.terminal_flags =
			MORPH_TUI_TERMINAL_SESSION_ALTERNATE | MORPH_TUI_TERMINAL_SESSION_PASTE,
		.tick_ms = 100,
	};
	int rc = morph_tui_terminal_available(stdin, stdout) ? 0 : -ENOTTY;

	sample.menu.list.flags = MORPH_TUI_WIDGET_LIST_VIM;
	if (!rc)
		rc = morph_buf_init(&sample.commands, BUFSIZ);
	if (!rc)
		rc = morph_buf_init(&sample.line, BUFSIZ);
	if (!rc)
		rc = morph_signal_listener_start(&signals, sample_minimal_signal, &sample);
	if (!rc)
		rc = morph_tui_core_app_create(&options, &app);
	if (!rc)
		rc = morph_tui_core_app_present(app, &controller);
	if (!rc)
		rc = morph_tui_core_app_run(app);
	morph_tui_core_app_destroy(app);
	morph_signal_listener_stop(&signals);
	morph_buf_cleanup(&sample.line);
	morph_buf_cleanup(&sample.commands);
	if (rc) {
		fprintf(stderr, "TUI sample failed: %s\n", morph_strerror(rc));
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
