#ifndef MORPH_CLI_PAGES_QUESTION_H
#define MORPH_CLI_PAGES_QUESTION_H

#include "agent/tools/ask_user.h"

struct cli_application;

int cli_question_callback(const char *question, const char *const *choices, int choices_count,
			  const char *selection_mode, int min_choices, int max_choices,
			  const struct ask_user_options *options, char ***answers,
			  int *answers_count, void *user_data);

#endif
