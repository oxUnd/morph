#ifndef MORPH_CLI_WIDGETS_COMPOSER_H
#define MORPH_CLI_WIDGETS_COMPOSER_H

#include <morph/tui/view.h>
#include <stdint.h>

struct cli_application;

/* Both returned views are borrowed for their controller's lifetime. */
struct morph_tui_view *cli_composer_add_borrow(struct morph_tui_builder *,
					       struct morph_tui_view *parent,
					       struct cli_application *,
					       struct morph_tui_view **editor_borrow);
int cli_composer_update(struct morph_tui_view *, int64_t now_ms);

#endif
