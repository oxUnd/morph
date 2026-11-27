#ifndef MORPH_CLI_COMPONENTS_CHAT_DOCK_H
#define MORPH_CLI_COMPONENTS_CHAT_DOCK_H

#include <nothing/tui/view.h>

struct cli_chat_dock_content {
	struct nt_view *back, *activity, *input, *footer;
	struct nt_view *composer, *editor, *panel;
};

struct nt_view *cli_chat_dock_add(struct nt_builder *, struct nt_view *parent);
struct nt_view *cli_chat_dock_input_add(struct nt_builder *, struct nt_view *parent);
int cli_chat_dock_set_content(struct nt_view *, const struct cli_chat_dock_content *);
int cli_chat_dock_fit(struct nt_view *, int width, int height, int separated);

#endif
