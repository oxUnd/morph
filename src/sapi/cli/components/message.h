#ifndef MORPH_CLI_COMPONENTS_MESSAGE_H
#define MORPH_CLI_COMPONENTS_MESSAGE_H

#include "sapi/cli/presentation/messages.h"
#include <nothing/tui/view.h>

struct nt_view *cli_message_add_borrow(struct nt_builder *, struct nt_view *parent,
				       enum cli_message_kind, const char *text, int styled);
int cli_message_set_text_copy(struct nt_view *, const char *text);
int cli_message_set_reasoning_expanded(struct nt_view *, int expanded);
int cli_message_set_ultra(struct nt_view *, int ultra);

#endif
