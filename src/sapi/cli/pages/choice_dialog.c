#include "sapi/cli/pages/choice_dialog.h"

#include "agent/tools/ask_user.h"
#include "sapi/cli/application.h"
#include "sapi/cli/components/question_form.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/style.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nothing/tui/models/selection_model.h>
#include <stdlib.h>
#include <string.h>

struct cli_choice_dialog {
	struct cli_choice_dialog_options options;
	struct ask_user_options preferences;
	struct nt_view *form;
	char *answer;
	size_t index;
	int result;
};

static void cli_choice_dialog_cleanup(struct cli_choice_dialog **owner)
{
	struct cli_choice_dialog *dialog = *owner;
	if (!dialog)
		return;

	free(dialog->answer);
	free(dialog);
	*owner = NULL;
}

static void cli_choice_dialog_submit(void *data, struct nt_view *view, int result)
{
	struct cli_choice_dialog *dialog = data;
	if (!result) {
		const struct nt_selection_model *model = cli_question_form_selection(view);
		for (size_t i = 0; i < nt_selection_model_count(model); i++) {
			if (nt_selection_model_is_selected(model, i))
				dialog->index = i;
		}

		const char *custom = cli_question_form_custom(view);
		if (custom) {
			dialog->answer = strdup(custom);
			if (!dialog->answer)
				result = NB_ERR_NOMEM;
		}
	}

	dialog->result = result;
	(void)cli_chat_panel_close(cli_application_from_view(view), 0);
}

static struct nt_view *cli_choice_dialog_build(struct nt_builder *builder, struct nt_view *parent,
					       void *data)
{
	struct cli_choice_dialog *dialog = data;
	dialog->form = cli_question_form_add(
		builder, parent,
		&(struct cli_question_form_options){.choices = dialog->options.choices,
						    .count = dialog->options.count,
						    .minimum = 1,
						    .maximum = 1,
						    .allow_custom = dialog->options.allow_custom,
						    .preferences = &dialog->preferences,
						    .complete = cli_choice_dialog_submit,
						    .data = dialog});
	return dialog->form ? cli_question_form_focus(dialog->form) : NULL;
}

static void cli_choice_dialog_complete(struct cli_application *ctx, void *data, int result)
{
	struct cli_choice_dialog *dialog NB_CLEANUP(cli_choice_dialog_cleanup) = data;
	dialog->options.complete(ctx, result ? result : dialog->result, dialog->index,
				 dialog->answer, dialog->options.data);
	if (dialog->options.release)
		dialog->options.release(dialog->options.data);
}

static int cli_choice_dialog_refresh(void *data, const struct nt_event *event)
{
	(void)event;
	return cli_question_form_refresh(((struct cli_choice_dialog *)data)->form);
}

int cli_choice_dialog_show(struct cli_application *ctx,
			   const struct cli_choice_dialog_options *opts)
{
	if (!ctx || !opts || !opts->complete || !cli_chat_active(ctx))
		return NB_ERR_INVAL;

	struct cli_choice_dialog *dialog NB_CLEANUP(cli_choice_dialog_cleanup) =
		calloc(1, sizeof(*dialog));
	if (!dialog)
		return NB_ERR_NOMEM;

	dialog->options = *opts;
	dialog->preferences.cancel_token = opts->cancel;
	dialog->result = NB_ERR_CANCELLED;
	dialog->index = SIZE_MAX;
	if (opts->question) {
		cli_presentation_prepare_prompt(ctx);
		cli_output_printf(ctx, CLI_FEED_QUESTION_STYLE CLI_FEED_QUESTION " ");
		(void)cli_output_print_untrusted(ctx, opts->question,
						 MORPH_TERMINAL_TEXT_SINGLE_LINE);
		cli_output_printf(ctx, ANSI_RESET "\n");
	}

	int rc = cli_chat_panel_mount(ctx, cli_choice_dialog_build, dialog,
				      cli_choice_dialog_complete);
	if (!rc) {
		dialog->options.choices = NULL;
		dialog->options.question = NULL;
		dialog = NULL;
		cli_chat_panel_set_interaction(ctx, CLI_CHAT_INTERACTION_CONFIRMATION);
		cli_chat_panel_set_refresh(ctx, cli_choice_dialog_refresh);
	}

	return rc;
}
