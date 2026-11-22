#ifndef MORPH_CLI_PAGES_CONFIRMATION_H
#define MORPH_CLI_PAGES_CONFIRMATION_H

struct cli_application;
typedef void (*cli_confirmation_complete_fn)(struct cli_application *, int accepted, void *data);

/* Native owner-thread command confirmation. A NULL phrase uses Cancel/Confirm;
 * otherwise the user must enter the exact phrase. Success owns data until release. */
int cli_confirmation_show(struct cli_application *, const char *phrase,
			  cli_confirmation_complete_fn, void *data, void (*release)(void *));

#endif
