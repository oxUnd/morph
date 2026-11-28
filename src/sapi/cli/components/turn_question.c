#include "sapi/cli/components/turn_question.h"

#include "sapi/cli/components/message.h"
#include "sapi/cli/components/report.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/presentation/turn_question.h"
#include "terminal/text.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/utf8.h>
#include <nothing/tui/views/scroll_view.h>
#include <nothing/tui/views/text_view.h>
#include <stdlib.h>

#define CLI_TURN_QUESTION_PREVIEW_CHARACTERS 512u

struct cli_turn_question {
	struct cli_report report;
	char *text;
	const struct nt_view *anchor;
	int pinned, ultra;
};

static int cli_turn_question_format(struct cli_report *report, nb_buf *out, int width)
{
	struct cli_turn_question *question = (struct cli_turn_question *)report;
	return question->anchor ? cli_turn_question_render(out, question->text, width,
		report->styled, question->ultra) : 0;
}

static int cli_turn_question_background(struct nt_view *view, struct nt_canvas *canvas)
{
	struct cli_turn_question *question = (struct cli_turn_question *)view;
	return question->report.styled ? nt_canvas_fill_styled(canvas, &canvas->style) : 0;
}

static int cli_turn_question_padding(struct nt_view *view, enum nt_view_edge edge,
	struct nt_canvas *canvas)
{
	(void)edge;
	return cli_turn_question_background(view, canvas);
}

static void cli_turn_question_destroy(struct nt_view *view)
{
	free(((struct cli_turn_question *)view)->text);
}

static const struct nt_view_class cli_turn_question_class = {
	.superclass = &cli_report_class,
	.instance_size = sizeof(struct cli_turn_question),
	.kind = "cli_turn_question",
	.deinit = cli_turn_question_destroy,
	.draw = cli_turn_question_background,
	.draw_padding = cli_turn_question_padding,
};

struct nt_view *cli_turn_question_add(struct nt_builder *builder, struct nt_view *parent, int styled)
{
	struct nt_view_options options = {.width = NT_FILL, .height = 1,
		.padding = {.right = 1}};
	if (styled)
		options.style.background = (struct nt_color){NT_COLOR_INDEXED, CLI_INPUT_BACKGROUND_INDEX};

	struct cli_turn_question *question = (struct cli_turn_question *)cli_report_add(
		builder, parent, &cli_turn_question_class, &options, styled, cli_turn_question_format);
	if (!question)
		return NULL;

	nt_builder_fail(builder, nt_text_view_set_wrap_mode(question->report.text, NT_TEXT_WRAP_NONE));
	return &question->report.base;
}

int cli_turn_question_sync(struct nt_view *view, struct nt_view *const *anchors, size_t count,
	const struct nt_view *scroll)
{
	if (!view || view->type != &cli_turn_question_class || (count && !anchors) || !scroll)
		return NB_ERR_INVAL;

	if (!nt_view_is_owner_thread(view))
		return NB_ERR_NOT_PERMITTED;

	const struct nt_scroll_state *state = nt_scroll_view_state(scroll);
	if (!state)
		return NB_ERR_INVAL;

	const struct nt_view *anchor = NULL;
	if (count) {
		size_t selected = count - 1;
		if (!state->follow) {
			int top = nt_view_content_rect(scroll).y;
			size_t first = 0, end = count;
			while (first < end) {
				size_t middle = first + (end - first) / 2;
				if (nt_view_frame(anchors[middle]).y <= top)
					first = middle + 1;
				else
					end = middle;
			}

			selected = first ? first - 1 : 0;
		}

		anchor = anchors[selected];
	}

	struct cli_turn_question *question = (struct cli_turn_question *)view;
	int pinned = 0;
	if (anchor) {
		struct nt_view_rect viewport = nt_view_content_rect(scroll);
		struct nt_view_rect source = nt_view_content_rect(anchor);
		/* Test visibility before reserving a row, avoiding a show/hide cycle
		 * when the original question sits at the viewport boundary. */
		size_t available = state->viewport + (size_t)nt_view_frame(view).height;
		int64_t top = state->follow
			? (state->content > available ? (int64_t)(state->content - available) : 0)
			: state->offset;
		int64_t bottom = (int64_t)source.y + source.height - viewport.y + state->offset;
		pinned = available >= 4 && viewport.width >= 16 && bottom <= top;
	}

	int changed = question->pinned != pinned;
	if (question->anchor == anchor) {
		if (changed) {
			question->pinned = pinned;
			nt_view_invalidate(view, 1);
		}

		return 0;
	}

	const char *text = anchor ? cli_message_user_text_borrow(anchor) : "";
	if (!text)
		return NB_ERR_INVAL;

	const char *end = nb_utf8_advance(text, CLI_TURN_QUESTION_PREVIEW_CHARACTERS);
	char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		text, (size_t)(end - text), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;

	free(question->text);
	question->text = nb_charp_steal(&safe);
	question->anchor = anchor;
	question->ultra = cli_message_user_is_ultra(anchor);
	question->pinned = pinned;
	cli_report_invalidate(&question->report, question->report.styled);
	return 0;
}

int cli_turn_question_is_pinned(const struct nt_view *view)
{
	return view && view->type == &cli_turn_question_class
		? ((const struct cli_turn_question *)view)->pinned : 0;
}
