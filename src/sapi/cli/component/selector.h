#ifndef MORPH_CLI_COMPONENT_SELECTOR_H
#define MORPH_CLI_COMPONENT_SELECTOR_H

#include <stddef.h>

struct cli_application;
struct cli_selector_choice {
	const char *label;
	const char *description;
};

/* Choices and title have static lifetime. Subtitle and detail are copied. */
int cli_selector_show(struct cli_application *, const char *title, const char *subtitle,
		      const char *detail, const struct cli_selector_choice *choices, size_t count,
		      int current, void (*apply)(struct cli_application *, size_t));

#endif
