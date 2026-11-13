#ifndef MORPH_CLI_PRESENTATION_MARKDOWN_H
#define MORPH_CLI_PRESENTATION_MARKDOWN_H

#include <limits.h>
#include "nb/buf.h"
#include "nb/cleanup.h"

struct cli_application;

/* Render a complete bounded document into a caller-owned buffer. No terminal
 * writes, media callbacks or presenter streaming state are involved. */
int cli_markdown_render_document(nb_buf *out, const char *text, int columns, unsigned int indent);
/* Captures native media/math as well as text for retained rich-document parsing. */
int cli_markdown_render_retained(nb_buf *out, const char *text, int columns, int styled);

struct cli_markdown_output {
	struct cli_application *ctx;
	char font_path[PATH_MAX];
	int prefix_pending;
	int trailing_newlines;
};

void cli_markdown_render_ansi(struct cli_application *ctx, const char *md);
typedef void (*cli_markdown_media_cb)(const char *type, const char *path, void *user);
void cli_markdown_render_ansi_with_media(struct cli_application *ctx, const char *md,
					 cli_markdown_media_cb cb, void *user);
void cli_markdown_render_ansi_with_media_indented(struct cli_application *ctx, const char *md,
						  unsigned int indent, cli_markdown_media_cb cb,
						  void *user);
void cli_markdown_render_final(struct cli_application *ctx, const char *text);
void cli_markdown_finish_final(struct cli_application *ctx);
int cli_markdown_stream_append(struct cli_application *ctx, const char *delta, int kind);
int cli_markdown_stream_refresh(struct cli_application *ctx);
void cli_markdown_stream_reset(struct cli_application *ctx, int finish_output);

#endif
