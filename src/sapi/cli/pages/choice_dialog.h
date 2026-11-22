#ifndef MORPH_CLI_PAGES_CHOICE_DIALOG_H
#define MORPH_CLI_PAGES_CHOICE_DIALOG_H

#include <stddef.h>

struct cli_application;
struct nb_cancel;

struct cli_choice_dialog_options {
	const char *question;
	const char *const *choices;
	int count, allow_custom;
	const struct nb_cancel *cancel;
	void (*complete)(struct cli_application *, int result, size_t index, const char *answer,
			 void *data);
	void *data;
	void (*release)(void *data);
};

/* Owner thread; native controls copy labels. Successful mounting owns data until
 * completion/release, including cancellation and application shutdown. */
int cli_choice_dialog_show(struct cli_application *, const struct cli_choice_dialog_options *);

#endif
