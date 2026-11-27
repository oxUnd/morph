#ifndef MORPH_CLI_COMPONENTS_EMPTY_STATE_H
#define MORPH_CLI_COMPONENTS_EMPTY_STATE_H

#include <nothing/tui/view.h>

struct cli_empty_state_action {
	const char *label;
	const char *description;
};

/* Text is copied by native controls; callback data is borrowed for view lifetime. */
struct nt_view *cli_empty_state_add(struct nt_builder *, struct nt_view *parent,
	const char *title, const char *description,
	const struct cli_empty_state_action *actions, size_t count, int styled,
	int (*activate)(void *data, size_t index), void *data,
	const struct nt_view_options *options);

#endif
