#ifndef MORPH_CLI_UI_PRESENTATION_MARKDOWN_H
#define MORPH_CLI_UI_PRESENTATION_MARKDOWN_H

struct cli_context;

void cli_markdown_render_ansi(struct cli_context *ctx, const char *md);
typedef void (*cli_markdown_media_cb)(const char *type, const char *path, void *user);
void cli_markdown_render_ansi_with_media(struct cli_context *ctx, const char *md,
					 cli_markdown_media_cb cb, void *user);
void cli_markdown_render_ansi_with_media_indented(struct cli_context *ctx, const char *md,
						  unsigned int indent, cli_markdown_media_cb cb,
						  void *user);
void cli_markdown_render_final(struct cli_context *ctx, const char *text);
void cli_markdown_finish_final(struct cli_context *ctx);
int cli_markdown_stream_append(struct cli_context *ctx, const char *delta, int kind);
void cli_markdown_stream_reset(struct cli_context *ctx, int finish_output);

#endif
