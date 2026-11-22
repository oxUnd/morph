#ifndef MORPH_CLI_COMPONENTS_QUESTION_FORM_H
#define MORPH_CLI_COMPONENTS_QUESTION_FORM_H

#include <nothing/tui/view.h>

struct ask_user_options;
struct nt_selection_model;

struct cli_question_form_options {
	const char *const *choices;
	int count, multi, minimum, maximum;
	int allow_custom;
	const struct ask_user_options *preferences;
	void (*complete)(void *data, struct nt_view *view, int result);
	void *data;
};

/* Native controls copy labels; preferences are borrowed until completion. */
struct nt_view *cli_question_form_add(struct nt_builder *, struct nt_view *,
				      const struct cli_question_form_options *);
struct nt_view *cli_question_form_focus(struct nt_view *);
const struct nt_selection_model *cli_question_form_selection(struct nt_view *);
const char *cli_question_form_custom(struct nt_view *);
int cli_question_form_refresh(struct nt_view *);

#endif
