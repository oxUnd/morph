#ifndef MORPH_CLI_PRESENTATION_OUTPUT_H
#define MORPH_CLI_PRESENTATION_OUTPUT_H
#include "util/buf.h"
#include "util/utf8.h"
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
void cli_output_print_padded(struct cli_application *ctx, const char *s, int target_width);
int cli_output_print_untrusted(struct cli_application *ctx, const char *text,
			       enum utf8_terminal_text_mode mode);
int cli_output_capture_begin(struct cli_application *ctx, morph_buf_t *output);
int cli_output_capture_styled_begin(struct cli_application *ctx, morph_buf_t *output);
void cli_output_capture_end(struct cli_application *ctx);
int cli_output_capture_write(struct cli_application *ctx, const char *bytes, size_t len);

#endif
