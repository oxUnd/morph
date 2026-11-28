#ifndef MORPH_CLI_COMPONENTS_TURN_QUESTION_H
#define MORPH_CLI_COMPONENTS_TURN_QUESTION_H

#include <nothing/tui/view.h>

struct nt_view *cli_turn_question_add(struct nt_builder *, struct nt_view *parent, int styled);
/* Anchors are borrowed user message views, ordered by their position in the feed. */
int cli_turn_question_sync(struct nt_view *, struct nt_view *const *anchors, size_t count,
	const struct nt_view *scroll);
int cli_turn_question_is_pinned(const struct nt_view *);

#endif
