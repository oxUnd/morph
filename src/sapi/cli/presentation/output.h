#ifndef MORPH_CLI_PRESENTATION_OUTPUT_H
#define MORPH_CLI_PRESENTATION_OUTPUT_H
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
#include <morph/tui/view.h>
#include <stdio.h>
struct cli_application;

void cli_output_set_color_enabled(struct cli_application *ctx, int enabled);
int cli_output_color_enabled(struct cli_application *ctx);
int cli_output_printf(struct cli_application *ctx, const char *fmt, ...);
int cli_output_fputs(struct cli_application *ctx, const char *text, FILE *stream);
int cli_output_putchar(struct cli_application *ctx, int ch);
#define CLI_OUTPUT_PRINTF cli_output_printf
#define CLI_OUTPUT_FPUTS cli_output_fputs
#define CLI_OUTPUT_PUTCHAR cli_output_putchar
int cli_output_write_json(const char *json);
void cli_output_set_structured(struct cli_application *ctx, int enabled);
int cli_output_is_structured(struct cli_application *ctx);
int cli_output_overlay_open(const struct cli_application *ctx);
int cli_output_overlay_visible(const struct cli_application *ctx);

int cli_output_result_begin(struct cli_application *ctx);
void cli_output_track_tail(int *tail, const char *bytes, size_t len);
/* Feed components own their box margin. The shared writer realizes it through
 * the TUI box model and reports how many rows are still missing. */
int cli_output_block_gap_margin(struct morph_tui_view_insets margin, int trailing_newlines);
/* Appends one committed block inside its box. The block's top margin is part
 * of the outer slot; the content starts below it. */
int cli_output_block_append(nb_buf *out, const char *text, struct morph_tui_view_insets margin,
			    int *trailing_newlines);
int cli_output_block_write(struct cli_application *ctx, const char *text,
			   struct morph_tui_view_insets margin);
void cli_output_print_padded(struct cli_application *ctx, const char *s, int target_width);
int cli_output_print_untrusted(struct cli_application *ctx, const char *text,
			       enum morph_terminal_text_mode mode);
int cli_output_capture_begin(struct cli_application *ctx, nb_buf *output);
int cli_output_capture_styled_begin(struct cli_application *ctx, nb_buf *output);
void cli_output_capture_end(struct cli_application *ctx);
int cli_output_capture_write(struct cli_application *ctx, const char *bytes, size_t len);

#endif
