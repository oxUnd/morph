#ifndef MORPH_CLI_WIDGETS_FEED_H
#define MORPH_CLI_WIDGETS_FEED_H

#include <stdio.h>
#include <stdint.h>

struct cli_application;

int cli_feed_init(struct cli_application *ctx, FILE *output, int output_fd);
void cli_feed_cleanup(struct cli_application *ctx);
void cli_feed_update_begin(struct cli_application *ctx);
void cli_feed_update_end(struct cli_application *ctx);
int cli_feed_update_active(const struct cli_application *ctx);
void cli_feed_turn_begin(struct cli_application *ctx);
void cli_feed_turn_end(struct cli_application *ctx, int result);
void cli_feed_live_set(struct cli_application *ctx, const char *text);
void cli_feed_reasoning_set(struct cli_application *ctx, const char *text);
void cli_feed_live_clear(struct cli_application *ctx);
void cli_feed_activity_set(struct cli_application *ctx, const char *text);
int64_t cli_feed_activity_elapsed_ms(const struct cli_application *ctx);
int cli_feed_columns(const struct cli_application *ctx);
void cli_feed_queue_changed(struct cli_application *ctx);
void cli_feed_render_frame(struct cli_application *ctx, int force);
int cli_feed_render_tick(struct cli_application *ctx, int cursor_rows);
int cli_feed_next_frame_ms(const struct cli_application *ctx);
void cli_feed_composer_suspend(struct cli_application *ctx);
void cli_feed_composer_resume(struct cli_application *ctx);
void cli_feed_history_begin(struct cli_application *ctx);
void cli_feed_history_end(struct cli_application *ctx);
void cli_feed_resize(struct cli_application *ctx);
void cli_feed_set_hidden(struct cli_application *ctx, int hidden);
int cli_feed_live_active(const struct cli_application *ctx);
int cli_feed_can_preview(const struct cli_application *ctx);
/* Renderer-owned ANSI text, already wrapped to the terminal content width. */
int cli_feed_markdown_preview_set(struct cli_application *ctx, const char *text,
				 size_t len, int prefix);

#endif
