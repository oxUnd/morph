#include "sapi/cli/presentation/markdown.h"
#include "morph_markdown_kitty.h"
#include "agent_ui/agent_ui.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/adapters/media/image_loader.h"
#include "sapi/cli/adapters/media/renderer.h"
#include "sapi/cli/application_internal.h"
#include "util/data.h"

#include <limits.h>
#include <sys/ioctl.h>

static int cli_markdown_write(const char *bytes, size_t len, void *user)
{
	struct cli_markdown_output *output = user;
	size_t i;
	int captured;

	if (!bytes || len == 0)
		return 0;
	/* The CLI synchronizes the entire feed/composer update. The SDK delivers
	 * complete frames; its inner end marker must not reveal a cleared input. */
	if (len >= 8 && memcmp(bytes, "\033[?2026h", 8) == 0) {
		bytes += 8;
		len -= 8;
	}
	if (len >= 8 && memcmp(bytes + len - 8, "\033[?2026l", 8) == 0)
		len -= 8;
	if (!len)
		return 0;
	if (output && cli_output_result_begin(output->ctx) != 0)
		return -EIO;
	/* The renderer buffers incomplete blocks. Emit the prefix only when it
	 * actually writes, so Readline cannot erase a dangling marker meanwhile. */
	if (output && output->prefix_pending) {
		printf(output->ctx, "%s" ANSI_BOLD "●" ANSI_RESET " ",
		       output->ctx->output.trailing_newlines < 2 ? "\n" : "");
		output->prefix_pending = 0;
	}
	if (output)
		cli_output_track_tail(&output->trailing_newlines, bytes, len);
	captured = cli_output_capture_write(output->ctx, bytes, len);
	if (captured)
		return captured < 0 ? captured : 0;
	if (output->ctx && !output->ctx->details_visible)
		cli_output_track_tail(&output->ctx->output.trailing_newlines, bytes, len);
	if (cli_output_color_enabled(output->ctx))
		return fwrite(bytes, 1u, len, stdout) == len ? 0 : -EIO;
	for (i = 0u; i < len;) {
		if ((unsigned char)bytes[i] == 0x1bu && i + 1u < len && bytes[i + 1u] == '[') {
			size_t end = i + 2u;

			while (end < len && ((unsigned char)bytes[end] < 0x40u ||
					     (unsigned char)bytes[end] > 0x7eu))
				end++;
			if (end < len && bytes[end] == 'm') {
				i = end + 1u;
				continue;
			}
		}
		if (fputc((unsigned char)bytes[i], stdout) == EOF)
			return -EIO;
		i++;
	}
	return 0;
}

static int cli_markdown_terminal_supports_kitty(void)
{
	const char *term;

	if (getenv("KITTY_WINDOW_ID"))
		return 1;
	term = getenv("TERM");
	return term && strstr(term, "kitty");
}

static struct morph_md_kitty *cli_markdown_create(struct cli_application *ctx, unsigned int indent,
						  unsigned int initial_column, int enable_math,
						  cli_markdown_media_cb cb, void *user,
						  struct cli_markdown_output *output)
{
	struct morph_md_kitty_options options;
	const char *font_path;

	output->ctx = ctx;
	memset(&options, 0, sizeof(options));
	font_path = morph_data_find(output->font_path, sizeof(output->font_path),
				    "fonts/STIXTwoMath-Regular.ttf") == 0
			    ? output->font_path
			    : NULL;
	options.font_path = font_path;
	options.features = MORPH_MD_FEATURE_GFM;
	if (enable_math && cli_output_color_enabled(ctx) &&
	    cli_markdown_terminal_supports_kitty() && font_path)
		options.features |= MORPH_MD_FEATURE_MATH;
	options.write = cli_markdown_write;
	options.user_data = output;
	options.media = cb;
	options.media_user_data = user;
	options.load_image = cli_image_loader_load;
	options.release_image = cli_image_loader_release;
	options.terminal_fd = STDOUT_FILENO;
	options.content_padding_left_columns = indent;
	options.content_padding_right_columns = CLI_CONTENT_RIGHT_PADDING;
	options.initial_cursor_column = initial_column;
	return morph_md_kitty_create(&options);
}

static int cli_markdown_render(struct cli_application *ctx, const char *md, unsigned int indent,
			       unsigned int initial_column, cli_markdown_media_cb cb, void *user)
{
	struct cli_markdown_output output = {0};
	struct morph_md_kitty *renderer;
	char *safe;
	char *normalized;
	const char *content;
	int rc;

	if (!md)
		return 0;
	safe = utf8_terminal_sanitize_dup(md, strlen(md), UTF8_TERMINAL_TEXT_MULTILINE, NULL);
	if (!safe)
		return 0;
	normalized = agent_ui_normalize_markdown(safe);
	content = normalized ? normalized : safe;
	renderer = cli_markdown_create(ctx, indent, initial_column, 1, cb, user, &output);
	if (!renderer) {
		log_warn(ctx ? ctx->logger : NULL, "failed to initialize Kitty Markdown renderer");
		goto out;
	}
	rc = morph_md_kitty_append(renderer, content, strlen(content), 1);
	if (rc == 0)
		rc = morph_md_kitty_render(renderer);
	if (rc != 0)
		log_warn(ctx ? ctx->logger : NULL, "Kitty Markdown rendering failed");
	morph_md_kitty_destroy(renderer);
	fflush(stdout);
out:
	free(normalized);
	free(safe);
	return output.trailing_newlines;
}

void cli_markdown_render_final(struct cli_application *ctx, const char *text)
{
	ctx->markdown_output.trailing_newlines =
		cli_markdown_render(ctx, text, 2u, 2u, cli_renderer_media, ctx);
}

void cli_markdown_finish_final(struct cli_application *ctx)
{
	while (ctx->markdown_output.trailing_newlines < 2) {
		putchar(ctx, '\n');
		ctx->markdown_output.trailing_newlines++;
	}
}

int cli_markdown_stream_append(struct cli_application *ctx, const char *delta, int kind)
{
	morph_buf_t safe;
	int rc;

	if (!ctx || !delta || !delta[0])
		return 0;
	if (ctx->markdown_stream && ctx->markdown_stream_kind != kind)
		cli_markdown_stream_reset(ctx, 1);
	if (!ctx->markdown_stream) {
		ctx->markdown_output.prefix_pending = 1;
		ctx->markdown_output.trailing_newlines = 0;
		ctx->markdown_stream = cli_markdown_create(ctx, 2u, 2u, 1, cli_renderer_media, ctx,
							   &ctx->markdown_output);
		if (!ctx->markdown_stream)
			return -ENOMEM;
		ctx->markdown_stream_kind = kind;
		ctx->markdown_stream_visible = 1;
		utf8_terminal_sanitizer_init(&ctx->markdown_stream_sanitizer,
					     UTF8_TERMINAL_TEXT_MULTILINE);
	}
	rc = morph_buf_init(&safe, strlen(delta) + 16);
	if (rc != 0)
		return rc;
	rc = utf8_terminal_sanitize_feed(&ctx->markdown_stream_sanitizer, &safe, delta,
					 strlen(delta), 0);
	if (rc == 0 && safe.len > 0)
		rc = morph_buf_append(&ctx->markdown_stream_text, morph_buf_cstr(&safe), safe.len);
	if (rc == 0 && safe.len > 0)
		rc = morph_md_kitty_append(ctx->markdown_stream, morph_buf_cstr(&safe), safe.len,
					   0);
	morph_buf_cleanup(&safe);
	if (rc != 0)
		return rc;
	return morph_md_kitty_render(ctx->markdown_stream);
}

void cli_markdown_stream_reset(struct cli_application *ctx, int finish_output)
{
	morph_buf_t safe;
	int safe_ready = 0;

	if (!ctx)
		return;
	if (finish_output && ctx->markdown_stream) {
		if (morph_buf_init(&safe, 16) == 0) {
			safe_ready = 1;
			if (utf8_terminal_sanitize_feed(&ctx->markdown_stream_sanitizer, &safe,
							NULL, 0, 1) == 0 &&
			    safe.len > 0) {
				(void)morph_buf_append(&ctx->markdown_stream_text,
						       morph_buf_cstr(&safe), safe.len);
				(void)morph_md_kitty_append(ctx->markdown_stream,
							    morph_buf_cstr(&safe), safe.len, 0);
			}
		}
		if (morph_md_kitty_append(ctx->markdown_stream, NULL, 0u, 1) == 0)
			(void)morph_md_kitty_render(ctx->markdown_stream);
	}
	if (safe_ready)
		morph_buf_cleanup(&safe);
	morph_md_kitty_destroy(ctx->markdown_stream);
	ctx->markdown_stream = NULL;
	ctx->markdown_stream_kind = 0;
	ctx->markdown_stream_visible = 0;
	ctx->markdown_output.prefix_pending = 0;
	morph_buf_reset(&ctx->markdown_stream_text);
	utf8_terminal_sanitizer_reset(&ctx->markdown_stream_sanitizer);
}

void cli_markdown_render_ansi(struct cli_application *ctx, const char *md)
{
	cli_markdown_render(ctx, md, 0u, 0u, NULL, NULL);
}

void cli_markdown_render_ansi_with_media(struct cli_application *ctx, const char *md,
					 cli_markdown_media_cb cb, void *user)
{
	cli_markdown_render(ctx, md, 0u, 0u, cb, user);
}

void cli_markdown_render_ansi_with_media_indented(struct cli_application *ctx, const char *md,
						  unsigned int indent, cli_markdown_media_cb cb,
						  void *user)
{
	cli_markdown_render(ctx, md, indent, indent, cb, user);
}
