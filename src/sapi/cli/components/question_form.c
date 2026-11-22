#include "sapi/cli/components/question_form.h"

#include "agent/tools/ask_user.h"
#include "sapi/cli/components/shortcut_hint.h"
#include "sapi/cli/presentation/style.h"
#include <nb/buf.h>
#include <nb/cancel.h>
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/time.h>
#include <nothing/tui/controls/choice_list.h>
#include <nothing/tui/controls/text_field.h>
#include <nothing/tui/theme.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/stack_view.h>
#include <stdlib.h>
#include <string.h>

struct cli_question_form {
	struct nt_stack_view base;
	struct cli_question_form_options options;
	struct nt_view *list, *summary, *editing_group, *editor, *validation, *keys, *hint;
	char *custom;
	int editing, finished;
	int64_t deadline;
};

static int cli_question_form_finish(struct cli_question_form *form, int result)
{
	if (!form->finished) {
		form->finished = 1;
		form->options.complete(form->options.data, &form->base.base, result);
	}

	return NT_VIEW_EVENT_STOP;
}

static int cli_question_form_summary(struct cli_question_form *form)
{
	struct nt_selection_model *model = nt_choice_list_model(form->list);
	const struct nt_choice_list_state *state = nt_choice_list_state(form->list);
	char text[128] = {0};
	if (model->multi)
		snprintf(text, sizeof(text), "  %zu selected · choose %zu-%zu%s",
			 model->selected_count, model->minimum, model->maximum,
			 model->invalid ? " · Selection limit" : "");
	else if (state->list.count > 8)
		snprintf(text, sizeof(text), "  %zu/%zu", state->list.index + 1, state->list.count);

	return nt_label_set_text(form->summary, text);
}

static int cli_question_form_edit(struct cli_question_form *form, int editing)
{
	int rc = nt_text_field_set_text(form->editor, form->custom ? form->custom : "");
	if (!rc)
		rc = nt_label_set_text(form->validation, "  Your answer");

	if (!rc)
		rc = nt_view_set_hidden(form->list, editing);

	if (!rc)
		rc = nt_view_set_hidden(form->summary, editing || (!form->options.multi &&
								   form->options.count < 8));

	if (!rc)
		rc = nt_view_set_hidden(form->keys, editing);

	if (!rc)
		rc = nt_view_set_hidden(form->editing_group, !editing);

	if (!rc) {
		form->editing = editing;
		rc = nt_view_focus(editing ? form->editor : form->list);
	}

	return rc;
}

static int cli_question_form_capture(struct nt_view *view, const struct nt_event *event)
{
	struct cli_question_form *form = (struct cli_question_form *)view;
	if (event->type != NT_EVENT_KEY && event->type != NT_EVENT_TEXT &&
	    event->type != NT_EVENT_PASTE)
		return 0;

	if (form->deadline) {
		form->deadline = 0;
		int rc = cli_shortcut_hint_set_text(form->hint,
						    "  Auto-continue paused while answering.");
		if (rc)
			return rc;
	}

	if (event->type != NT_EVENT_KEY || event->pasting)
		return 0;

	if (event->key == 3 || event->key == 4)
		return cli_question_form_finish(form, NB_ERR_CANCELLED);

	if (event->key == 27) {
		if (form->editing && form->options.count) {
			int rc = cli_question_form_edit(form, 0);
			return rc ? rc : NT_VIEW_EVENT_STOP;
		}

		return cli_question_form_finish(form, NB_ERR_CANCELLED);
	}

	if (form->options.allow_custom && !form->editing &&
	    nt_choice_list_selected(form->list) == (size_t)form->options.count &&
	    (event->key == 'e' ||
	     ((event->key == '\r' || event->key == '\n' || event->key == ' ') &&
	      (!form->custom || !form->options.multi)))) {
		int rc = cli_question_form_edit(form, 1);
		return rc ? rc : NT_VIEW_EVENT_STOP;
	}

	return 0;
}

static int cli_question_form_action(void *data, const struct nt_view_event *event)
{
	struct cli_question_form *form = data;
	if (event->target == form->editor && event->type == NT_VIEW_EVENT_SUBMIT) {
		const char *text = event->text ? event->text : "";
		if (!text[0]) {
			int rc = nt_label_set_text(form->validation, "  Enter a non-empty answer.");
			return rc ? rc : NT_VIEW_EVENT_STOP;
		}

		char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(text);
		if (!copy)
			return NB_ERR_NOMEM;

		free(form->custom);
		form->custom = nb_charp_steal(&copy);
		int rc = nt_selection_model_set_selected(nt_choice_list_model(form->list),
							 (size_t)form->options.count, 1);
		if (rc && rc != NB_ERR_RANGE)
			return rc;

		if (!form->options.multi)
			return cli_question_form_finish(form, 0);

		rc = cli_question_form_edit(form, 0);
		if (!rc)
			rc = cli_question_form_summary(form);

		nt_view_invalidate(form->list, 0);
		return rc ? rc : NT_VIEW_EVENT_STOP;
	}

	if (event->target != form->list)
		return 0;

	if (form->options.allow_custom && event->type == NT_VIEW_EVENT_CHANGED && !form->custom &&
	    event->index == (size_t)form->options.count &&
	    nt_choice_list_is_checked(form->list, event->index)) {
		int rc = nt_selection_model_set_selected(nt_choice_list_model(form->list),
							 event->index, 0);
		if (!rc)
			rc = cli_question_form_edit(form, 1);

		return rc ? rc : NT_VIEW_EVENT_STOP;
	}

	if (event->type == NT_VIEW_EVENT_SUBMIT)
		return cli_question_form_finish(form, 0);

	if (event->type == NT_VIEW_EVENT_CHANGED ||
	    event->type == NT_VIEW_EVENT_SELECTION_CHANGED ||
	    event->type == NT_VIEW_EVENT_INVALID) {
		int rc = cli_question_form_summary(form);
		return rc ? rc : NT_VIEW_EVENT_HANDLED;
	}

	return 0;
}

static int cli_question_form_row(void *data, struct nt_canvas *canvas, size_t index,
				 const char *label, int focused, int checked)
{
	struct cli_question_form *form = data;
	struct nt_text_style style = nt_theme_style(
		canvas->theme, focused ? NT_THEME_ACCENT : NT_THEME_TEXT, NT_VISUAL_NORMAL);
	if (focused)
		style.attributes |= NT_STYLE_BOLD;

	const struct ask_user_options *preferences = form->options.preferences;
	int recommended = 0;
	for (int i = 0; preferences && i < preferences->recommended_count; i++)
		recommended |= (size_t)preferences->recommended_indices[i] == index;

	if (index == (size_t)form->options.count && form->custom)
		label = form->custom;

	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!nb_buf_printf(&text, "  %s %s %zu. %s%s", focused ? "›" : " ",
			   form->options.multi ? (checked ? "[x]" : "[ ]")
					       : (focused ? CLI_FEED_DONE : CLI_FEED_PENDING),
			   index + 1, label, recommended ? " (Recommended)" : ""))
		return nb_buf_status(&text);

	int rc = nt_canvas_fill_styled(canvas, &style);
	return rc ? rc : nt_canvas_text_styled(canvas, 0, 0, nb_buf_cstr(&text), &style);
}

static void cli_question_form_deinit(struct nt_view *view)
{
	free(((struct cli_question_form *)view)->custom);
}

struct nt_view *cli_question_form_add(struct nt_builder *builder, struct nt_view *parent,
				      const struct cli_question_form_options *options)
{
	static const struct nt_view_class type = {
		.superclass = &nt_stack_view_class,
		.instance_size = sizeof(struct cli_question_form),
		.kind = "cli_question_form",
		.capture_event = cli_question_form_capture,
		.deinit = cli_question_form_deinit,
	};
	if (!options || options->count < 0 || options->count > ASK_USER_CHOICES_MAX ||
	    (!options->count && !options->allow_custom) || !options->complete) {
		nt_builder_fail(builder, NB_ERR_INVAL);
		return NULL;
	}

	int count = options->count + (options->allow_custom != 0);
	int rows = count < 8 ? count : 8;
	struct cli_question_form *form = (struct cli_question_form *)nt_view_add(
		builder, parent, &type,
		&(struct nt_view_options){.width = NT_FILL,
					  .margin = {.top = 1, .bottom = 1},
					  .min_height = rows + 3});
	if (!form)
		return NULL;

	form->options = *options;
	const char *items[ASK_USER_CHOICES_MAX + 1] = {0};
	for (int i = 0; i < options->count; i++)
		items[i] = options->choices[i];

	if (options->allow_custom)
		items[options->count] = "Other · Type your own answer";
	struct nt_view *root = &form->base.base;
	form->list = nt_choice_list_add(builder, root, items, (size_t)count, options->multi,
					(size_t)options->minimum, (size_t)options->maximum, 0,
					&(struct nt_view_options){.height = rows});
	form->summary = nt_label_add(builder, root, "", &(struct nt_view_options){.height = 1});
	form->editing_group = nt_column_add(builder, root, 0, NULL);
	form->validation = nt_label_add(builder, form->editing_group, "  Your answer", NULL);
	form->editor =
		nt_text_field_add(builder, form->editing_group,
				  &(struct nt_text_field_options){
					  .prompt = "  ", .limit = ASK_USER_ANSWER_MAX_BYTES + 1},
				  &(struct nt_view_options){.height = 1});
	cli_shortcut_hint_add(builder, form->editing_group,
			      "  Enter Save · Esc Back · Ctrl+U Clear", NULL);
	form->keys = cli_shortcut_hint_add(
		builder, root,
		options->multi ? "  ↑↓ Move · Space toggle · Enter confirm · E Edit · Esc Cancel"
			       : "  ↑↓ Move · Enter confirm · Esc Cancel",
		NULL);
	char hint[160];
	const struct ask_user_options *preferences = options->preferences;
	int timeout = preferences && preferences->optional ? preferences->timeout_seconds : 0;
	if (timeout)
		snprintf(hint, sizeof(hint), "  Auto-continue after %d s without input%s.", timeout,
			 preferences->recommended_count ? " using recommended choices" : "");
	else
		snprintf(hint, sizeof(hint), "  Waiting for your answer · Esc Cancel");

	form->hint = cli_shortcut_hint_add(builder, root, hint, NULL);
	if (nt_builder_error(builder))
		return NULL;

	int rc = nt_choice_list_set_navigation(form->list, NT_LIST_VIEW_WRAP | NT_LIST_VIEW_VIM |
								   NT_LIST_VIEW_TAB |
								   NT_LIST_VIEW_DIGITS);
	if (!rc)
		rc = nt_choice_list_set_row_renderer(form->list, cli_question_form_row, form);

	if (!rc)
		rc = nt_view_set_hidden(form->editing_group, options->count != 0);

	if (!rc)
		rc = nt_view_set_hidden(form->list, options->count == 0);

	if (!rc)
		rc = nt_view_set_hidden(form->summary, !options->multi && count <= 8);

	if (!rc)
		rc = nt_view_set_hidden(form->keys, options->count == 0);

	for (int i = 0; !rc && preferences && i < preferences->recommended_count; i++) {
		size_t index = (size_t)preferences->recommended_indices[i];
		if (options->multi)
			rc = nt_selection_model_set_selected(nt_choice_list_model(form->list),
							     index, 1);

		if (!rc && !i)
			rc = nt_choice_list_select(form->list, index);
	}

	if (!rc && !nt_view_on(root, NT_VIEW_EVENT_ACTION, cli_question_form_action, form))
		rc = NB_ERR_NOMEM;

	if (!rc)
		rc = cli_question_form_summary(form);

	form->editing = options->count == 0;
	form->options.choices = NULL;
	form->deadline = timeout ? nb_time_monotonic_ms() + (int64_t)timeout * 1000 : 0;
	nt_builder_fail(builder, rc);
	return rc ? NULL : root;
}

struct nt_view *cli_question_form_focus(struct nt_view *view)
{
	struct cli_question_form *form = (struct cli_question_form *)view;
	return form->editing ? form->editor : form->list;
}

const struct nt_selection_model *cli_question_form_selection(struct nt_view *view)
{
	return nt_choice_list_model(((struct cli_question_form *)view)->list);
}

const char *cli_question_form_custom(struct nt_view *view)
{
	return ((struct cli_question_form *)view)->custom;
}

int cli_question_form_refresh(struct nt_view *view)
{
	struct cli_question_form *form = (struct cli_question_form *)view;
	const struct ask_user_options *preferences = form->options.preferences;
	if (preferences && nb_cancel_requested(preferences->cancel_token))
		return cli_question_form_finish(form, NB_ERR_CANCELLED);

	if (form->deadline && nb_time_monotonic_ms() >= form->deadline)
		return cli_question_form_finish(form, NB_ERR_TIMEOUT);

	return 0;
}
