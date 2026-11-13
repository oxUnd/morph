#ifndef MORPH_CLI_WIDGETS_COMPOSER_PANEL_H
#define MORPH_CLI_WIDGETS_COMPOSER_PANEL_H

#include "nb/buf.h"
#include "nb/cleanup.h"
#include <morph/tui/event.h>
#include <morph/tui/view.h>

struct cli_application;

struct cli_composer_panel_options {
	int max_rows;
	int tick_ms;
};

/* Append through the line helpers. Each line occupies one clipped physical row. */
struct cli_composer_panel_frame {
	nb_buf content;
	int columns;
	int rows;
	int line_count;
	int color;
	int error;
};

struct cli_composer_panel_ops {
	int (*draw)(void *data, struct cli_composer_panel_frame *frame);
	/* Returns MORPH_TUI_SCREEN_CONTINUE, REDRAW, CLOSE, or a negative error. */
	int (*update)(void *data, const struct morph_tui_event *event);
	void (*complete)(void *data, int result);
};

/* Replaces the composer from its upper edge downward. The component owns the
 * transient command separator and space reservation; callers draw content only.
 * Success retains data until complete; failure leaves it with the caller. */
int cli_composer_panel_show(struct cli_application *application,
			    const struct cli_composer_panel_options *options,
			    const struct cli_composer_panel_ops *ops, void *data);
struct morph_tui_view *cli_composer_panel_add_borrow(struct morph_tui_builder *,
	struct morph_tui_view *parent, struct cli_application *,
	const struct cli_composer_panel_options *, const struct cli_composer_panel_ops *, void *data);
int cli_composer_panel_update(struct morph_tui_view *, const struct morph_tui_event *);
int cli_composer_panel_finish(struct morph_tui_view *, int result);
int cli_composer_panel_line(struct cli_composer_panel_frame *frame, const char *text,
			    const char *style);
/* Trusted renderer output may contain SGR. Cursor controls and OSC are removed. */
int cli_composer_panel_styled_line(struct cli_composer_panel_frame *frame, const char *text);

#endif
