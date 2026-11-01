#ifndef MORPH_CLI_CORE_H
#define MORPH_CLI_CORE_H

#include "util/buf.h"
#include "util/utf8.h"
#include <stdint.h>

struct cli_context;

int cli_command_result_begin(struct cli_context *ctx);
void cli_output_track_tail(int *tail, const char *bytes, size_t len);
void cli_print_padded(struct cli_context *ctx, const char *s, int target_width);
int cli_print_untrusted_text(struct cli_context *ctx, const char *text,
			     enum utf8_terminal_text_mode mode);
int cli_command_capture_begin(struct cli_context *ctx, morph_buf_t *output);
int cli_command_capture_styled_begin(struct cli_context *ctx, morph_buf_t *output);
void cli_command_capture_end(struct cli_context *ctx);
int cli_command_capture_write(struct cli_context *ctx, const char *bytes, size_t len);
void cli_record_media_credits(struct cli_context *ctx, const char *kind, int64_t image_units,
			      int64_t video_seconds, const char *provider, const char *model,
			      const char *metadata_json);
void cli_turn_begin(struct cli_context *ctx);
void cli_turn_finish(struct cli_context *ctx, int turn_rc);

#endif
