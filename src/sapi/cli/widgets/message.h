#ifndef MORPH_CLI_WIDGETS_MESSAGE_H
#define MORPH_CLI_WIDGETS_MESSAGE_H

#include "sapi/cli/presentation/messages.h"
#include <morph/tui/widget.h>

struct morph_tui_view *cli_message_add_borrow(struct morph_tui_builder *,
					      struct morph_tui_view *parent, enum cli_message_kind,
					      const char *text, int styled);
int cli_message_set_text_copy(struct morph_tui_view *, const char *text);
int cli_message_set_reasoning_expanded(struct morph_tui_view *, int expanded);
int cli_message_set_ultra(struct morph_tui_view *, int ultra);

#endif
