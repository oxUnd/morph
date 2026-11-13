#include <morph/tui/widgets/select.h>
#include "nb/time.h"
#include "nb/err.h"
#include <morph/tui/widgets/editor.h>
#include <morph/tui/screen.h>
#include <morph/tui/draw.h>
#include "nb/signal.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
#include "nb/cancel.h"

#include <stdlib.h>
#include <string.h>

#define WIDGET_SELECT_PROMPT_BOLD "\033[1m"
#define WIDGET_SELECT_PROMPT_CYAN "\033[36m"
#define WIDGET_SELECT_PROMPT_DIM "\033[2m"
#define WIDGET_SELECT_PROMPT_RED "\033[31m"
#define WIDGET_SELECT_PROMPT_RESET "\033[0m"

struct widget_select_prompt_view {
	FILE *output;
	int color;
	int allow_custom;
	struct morph_tui_widget_select *state;
	const char *const *choices;
	int columns;
	int rows;
	int active_lines;
	size_t active_width;
	int cursor_up;
	int editing;
	int input_invalid;
	struct morph_tui_widget_editor_state editor;
	char *answer;
	const char *hint;
	int timeout_paused;
	size_t answer_limit;
};

static void widget_select_prompt_clear(struct widget_select_prompt_view *view, nb_buf *frame,
				       int columns, int rows)
{
	if (view->active_lines) {
		if (view->cursor_up)
			(void)nb_buf_printf(frame, "\033[%dB", view->cursor_up);
		if ((columns != view->columns && (size_t)columns <= view->active_width) ||
		    rows != view->rows || view->active_lines >= rows) {
			/* Reflow may have moved old rows into scrollback. */
			(void)nb_buf_append_cstr(frame, "\033[999B\r\n");
		} else {
			(void)nb_buf_printf(frame, "\r\033[%dA\033[J", view->active_lines);
		}
	}
	view->columns = columns;
	view->rows = rows;
	view->active_lines = 0;
	view->active_width = 0;
	view->cursor_up = 0;
}

static void widget_select_prompt_line(struct widget_select_prompt_view *view, nb_buf *frame,
				      const char *text, const char *style)
{
	char clipped[BUFSIZ];
	size_t width = view->columns > 1 ? (size_t)view->columns - 1 : 0;
	char *safe = morph_terminal_sanitize_dup(text, strlen(text),
						 MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);

	if (!safe) {
		frame->error = NB_ERR_NOMEM;
		return;
	}
	morph_terminal_copy_sanitized_display_width(clipped, sizeof(clipped), safe, width);
	free(safe);
	size_t used = nb_utf8_width_cstr(clipped);

	if (used > view->active_width)
		view->active_width = used;
	(void)nb_buf_printf(frame, "%s%s%s\r\n", view->color ? style : "", clipped,
			    view->color ? WIDGET_SELECT_PROMPT_RESET : "");
	view->active_lines++;
}

static int widget_select_prompt_draw(struct widget_select_prompt_view *view,
				     const struct morph_tui_screen *screen)
{
	nb_buf frame = {0}, label = {0};
	int rc = (nb_buf_init(&frame), nb_buf_reserve(&frame, BUFSIZ) ? 0 : NB_ERR_NOMEM);

	if (!rc)
		rc = (nb_buf_init(&label), nb_buf_reserve(&label, 128) ? 0 : NB_ERR_NOMEM);
	if (rc) {
		nb_buf_cleanup(&frame);
		return rc;
	}
	(void)nb_buf_append_cstr(&frame, "\033[?2026h");
	widget_select_prompt_clear(view, &frame, screen->area.width, screen->area.height);
	(void)nb_buf_append_cstr(&frame, "\033[?25l");
	struct morph_tui_widget_select *state = view->state;
	struct morph_tui_widget_list_state *list = &state->menu.list;
	if (view->editing) {
		widget_select_prompt_line(
			view, &frame,
			view->input_invalid ? "  Enter a non-empty answer; shorten it if needed."
					    : "  Your answer",
			view->input_invalid ? WIDGET_SELECT_PROMPT_RED : WIDGET_SELECT_PROMPT_BOLD);
		size_t column = 0;
		size_t width = view->columns > 3 ? (size_t)view->columns - 3 : 1;

		rc = morph_tui_widget_editor_state_view(&view->editor, &label, width, &column);
		if (!rc) {
			nb_buf input NB_CLEANUP(nb_buf_cleanup) = {0};
			rc = (nb_buf_init(&input), nb_buf_reserve(&input, 128) ? 0 : NB_ERR_NOMEM);
			if (!rc)
				rc = (nb_buf_printf(&input, "  %s", nb_buf_cstr(&label))
					      ? 0
					      : NB_ERR_NOMEM);
			if (!rc)
				widget_select_prompt_line(view, &frame, nb_buf_cstr(&input),
							  WIDGET_SELECT_PROMPT_CYAN);
			nb_buf_cleanup(&input);
		}
		if (!rc) {
			widget_select_prompt_line(view, &frame,
						  "  Enter save · Esc back · Ctrl+U clear",
						  WIDGET_SELECT_PROMPT_DIM);
			view->cursor_up = 2;
			(void)nb_buf_printf(&frame, "\033[2A\r\033[%zuC\033[?25h", column + 2);
		}
	} else {
		int page = screen->area.height > 4 ? screen->area.height - 4 : 1;

		if (page > 8)
			page = 8;
		morph_tui_widget_list_state_layout(list, list->count, (size_t)page);
		for (size_t i = list->first; i < list->count && i - list->first < list->page; i++) {
			nb_buf_clear(&label);
			(void)nb_buf_printf(
				&label, "  %s %s %zu. %s", i == list->index ? "›" : " ",
				state->multi ? (morph_tui_widget_select_selected(state, i) ? "[x]"
											   : "[ ]")
					     : (i == list->index ? "●" : "○"),
				i + 1,
				view->allow_custom && i + 1 == list->count
					? (view->answer ? view->answer
							: "Other · Type your own answer")
					: view->choices[i]);
			if (nb_buf_status(&label))
				frame.error = nb_buf_status(&label);
			widget_select_prompt_line(
				view, &frame, nb_buf_cstr(&label),
				i == list->index
					? WIDGET_SELECT_PROMPT_BOLD WIDGET_SELECT_PROMPT_CYAN
					: "");
		}
		nb_buf_clear(&label);
		if (state->multi)
			(void)nb_buf_printf(&label, "  %zu selected · choose %zu-%zu%s",
					    state->selected_count, state->minimum, state->maximum,
					    state->invalid ? " · Selection limit" : "");
		else if (list->count > list->page)
			(void)nb_buf_printf(&label, "  %zu/%zu", list->index + 1, list->count);
		if (label.len && screen->area.height > 3)
			widget_select_prompt_line(view, &frame, nb_buf_cstr(&label),
						  state->invalid ? WIDGET_SELECT_PROMPT_RED
								 : WIDGET_SELECT_PROMPT_DIM);
		if (screen->area.height > 2)
			widget_select_prompt_line(
				view, &frame,
				state->multi
					? (view->allow_custom ? "  ↑↓ Move · Space toggle · Enter "
								"confirm · e edit · Esc cancel"
							      : "  ↑↓ Move · Space toggle · Enter "
								"confirm · Esc cancel")
					: "  ↑↓ Move · Enter confirm · Esc cancel",
				WIDGET_SELECT_PROMPT_DIM);
	}
	if (view->hint)
		widget_select_prompt_line(view, &frame,
					  view->timeout_paused
						  ? "  Auto-continue paused while answering."
						  : view->hint,
					  WIDGET_SELECT_PROMPT_DIM);
	if (nb_buf_status(&label))
		frame.error = nb_buf_status(&label);
	if (rc)
		frame.error = rc;
	(void)nb_buf_append_cstr(&frame, "\033[?2026l");
	rc = morph_tui_draw_write(view->output, &frame);
	nb_buf_cleanup(&label);
	nb_buf_cleanup(&frame);
	return rc;
}

static int widget_select_prompt_update(struct widget_select_prompt_view *view, int key)
{
	struct morph_tui_widget_select *state = view->state;
	int result;

	if (view->editing) {
		result = morph_tui_widget_editor_state_update(&view->editor, key);
		if (result == MORPH_TUI_WIDGET_BACK) {
			view->editing = 0;
			result = MORPH_TUI_WIDGET_CHANGED;
		} else if (result == MORPH_TUI_WIDGET_SUBMIT) {
			view->input_invalid = !morph_tui_widget_editor_state_valid(&view->editor);
			if (!view->input_invalid) {
				char *answer = strdup(nb_buf_cstr(&view->editor.text));

				if (!answer) {
					return NB_ERR_NOMEM;
				}
				free(view->answer);
				view->answer = answer;
				view->editing = 0;
				if (!state->multi)
					return MORPH_TUI_WIDGET_SUBMIT;
				if (!morph_tui_widget_select_selected(state,
								      state->menu.list.index))
					(void)morph_tui_widget_select_update(state, ' ');
			}
			result = MORPH_TUI_WIDGET_CHANGED;
		}
	} else if (view->allow_custom && !state->menu.pasting &&
		   state->menu.list.index + 1 == state->menu.list.count &&
		   (key == 'e' || ((key == '\r' || key == '\n' || key == ' ') &&
				   (!view->answer || !state->multi)))) {
		morph_tui_widget_editor_state_cleanup(&view->editor);
		int rc = morph_tui_widget_editor_state_init(
			&view->editor, view->answer ? view->answer : "", view->answer_limit, 0);
		if (rc)
			return rc;
		view->editing = 1;
		view->input_invalid = 0;
		result = MORPH_TUI_WIDGET_CHANGED;
	} else {
		result = morph_tui_widget_select_update(state, key);
	}
	return result;
}

int morph_tui_widget_select_run(struct morph_tui_widget_select *state,
				const struct morph_tui_widget_select_options *options, FILE *input,
				FILE *output, char **custom)
{
	if (!state || !options || !options->items || !input || !output || !state->checked.len ||
	    (options->allow_custom && !custom))
		return NB_ERR_INVAL;
	if (custom)
		*custom = NULL;
	struct morph_tui_screen screen = {0};
	struct widget_select_prompt_view view = {
		.output = output,
		.color = options->color,
		.allow_custom = options->allow_custom,
		.state = state,
		.choices = options->items,
		.hint = options->hint,
		.answer_limit = options->custom_max_bytes ? options->custom_max_bytes + 1
							: 1024u * 1024u + 1,
	};
	if (!view.answer_limit)
		return NB_ERR_RANGE;
	int rc = morph_tui_widget_editor_state_init(&view.editor, "", view.answer_limit, 0);

	if (rc)
		return rc;
	rc = morph_tui_screen_open(&screen, input, output, MORPH_TUI_TERMINAL_SESSION_PASTE);
	if (rc) {
		morph_tui_widget_editor_state_cleanup(&view.editor);
		return rc;
	}
	screen.interrupted = options->interrupted;
	int64_t deadline =
		options->timeout_seconds > 0
			? nb_time_monotonic_ms() + (int64_t)options->timeout_seconds * 1000
			: 0;
	if (options->allow_custom && state->menu.list.count == 1)
		view.editing = 1;
	int first_frame = 1;
	for (;;) {
		if (nb_cancel_requested(options->cancel_token)) {
			rc = NB_ERR_CANCELLED;
			break;
		}
		if (first_frame) {
			rc = widget_select_prompt_draw(&view, &screen);
			if (rc)
				break;
			screen.dirty = 0;
			first_frame = 0;
		}
		struct morph_tui_event event;

		/* Drain available input before repainting, including unbracketed
		 * text bursts. Per-byte redraw can fill output while the sender is
		 * blocked writing a long UTF-8 answer to the same terminal. */
		rc = morph_tui_screen_poll(&screen, &event,
					   screen.dirty && !screen.pasting ? 0 : 100);
		if (rc == NB_ERR_INTERRUPTED && !nb_atomic_int_load(options->interrupted))
			continue;
		if (rc)
			break;
		if (event.type == MORPH_TUI_EVENT_EOF) {
			rc = NB_ERR_CANCELLED;
			break;
		}
		if (event.type != MORPH_TUI_EVENT_KEY) {
			if (event.type == MORPH_TUI_EVENT_TICK && screen.dirty && !screen.pasting) {
				rc = widget_select_prompt_draw(&view, &screen);
				if (rc)
					break;
				screen.dirty = 0;
			}
			if (deadline && nb_time_monotonic_ms() >= deadline) {
				rc = NB_ERR_TIMEOUT;
				break;
			}
			continue;
		}
		if (deadline) {
			deadline = 0;
			view.timeout_paused = 1;
			screen.dirty = 1;
		}
		int was_invalid = view.input_invalid || state->invalid;
		int result = widget_select_prompt_update(&view, event.key);

		if (result == MORPH_TUI_WIDGET_SUBMIT)
			break;
		if (result == MORPH_TUI_WIDGET_BACK || result == MORPH_TUI_WIDGET_CANCEL) {
			rc = NB_ERR_CANCELLED;
			break;
		}
		if (result < 0) {
			rc = result;
			break;
		}
		screen.dirty = screen.dirty || result == MORPH_TUI_WIDGET_CHANGED;
		/* Publish a rejection once even if subsequent editing keys are already
		 * queued. Successful text bursts still redraw only after input drains. */
		if (!screen.pasting && !was_invalid && (view.input_invalid || state->invalid)) {
			rc = widget_select_prompt_draw(&view, &screen);
			if (rc)
				break;
			screen.dirty = 0;
		}
	}
	nb_buf ending NB_CLEANUP(nb_buf_cleanup) = {0};
	int ending_rc = (nb_buf_init(&ending), nb_buf_reserve(&ending, 128) ? 0 : NB_ERR_NOMEM);

	if (!ending_rc) {
		widget_select_prompt_clear(&view, &ending, screen.area.width, screen.area.height);
		ending_rc = morph_tui_draw_write(output, &ending);
	}
	nb_buf_cleanup(&ending);
	int leave_rc = morph_tui_screen_close(&screen);

	if (!rc)
		rc = ending_rc ? ending_rc : leave_rc;
	if (!rc && view.allow_custom &&
	    morph_tui_widget_select_selected(state, state->menu.list.count - 1)) {
		*custom = view.answer;
		view.answer = NULL;
	}
	free(view.answer);
	morph_tui_widget_editor_state_cleanup(&view.editor);
	return rc;
}
