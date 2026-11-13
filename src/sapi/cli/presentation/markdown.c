#include "sapi/cli/presentation/markdown.h"
#include "nb/err.h"
#include "sapi/cli/application.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/presentation/style.h"
#include "nb/cleanup.h"
#include "sapi/cli/presentation/turn_details.h"
#include "sapi/cli/presentation/presenter_state.h"
#include "morph_markdown_kitty.h"
#include "agent_ui/agent_ui.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/adapters/media/image_loader.h"
#include "sapi/cli/adapters/media/renderer.h"
#include "sapi/cli/application_internal.h"
#include "runtime/resources.h"
#include <morph/tui/media.h>
#include "render/video.h"

#include <limits.h>
#include <sys/ioctl.h>

#define CLI_MARKDOWN_IMAGE_MAX_COLUMNS 48u
#define CLI_MARKDOWN_IMAGE_MAX_ROWS 12u

static int cli_markdown_terminal_supports_kitty(void);
static int cli_markdown_retained_document(nb_buf *, const char *, int, int, int);

static char *cli_markdown_normalized_copy(const char *text)
{
	char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		text, strlen(text), MORPH_TERMINAL_TEXT_MULTILINE, NULL);
	if (!safe)
		return NULL;
	char *normalized = agent_ui_normalize_markdown(safe);
	return normalized ? normalized : nb_charp_steal(&safe);
}

static void cli_markdown_options(struct morph_md_kitty_options *options, unsigned int indent,
				  unsigned int initial_column)
{
	memset(options, 0, sizeof(*options));
	options->features = MORPH_MD_FEATURE_GFM;
	options->content_padding_left_columns = indent;
	options->content_padding_right_columns = CLI_CONTENT_RIGHT_PADDING;
	options->initial_cursor_column = initial_column;
}

static int cli_markdown_document_write(const char *bytes, size_t length, void *opaque)
{
	return (nb_buf_append(opaque, bytes, length) ? 0 : NB_ERR_NOMEM);
}

static int cli_markdown_document_image(const char *url, char **path, void *opaque)
{
	(void)url;
	(void)opaque;
	*path = NULL;
	return NB_ERR_NOT_FOUND;
}

static void cli_markdown_renderer_cleanup(struct morph_md_kitty **owner)
{
	if (owner && *owner) {
		morph_md_kitty_destroy(*owner);
		*owner = NULL;
	}
}

int cli_markdown_render_document(nb_buf *out, const char *text, int columns, unsigned int indent)
{
	if (!out || !text)
		return NB_ERR_INVAL;
	char *normalized NB_CLEANUP(nb_charp_cleanup) = cli_markdown_normalized_copy(text);
	if (!normalized)
		return NB_ERR_NOMEM;
	struct morph_md_kitty_options options;
	cli_markdown_options(&options, indent, indent);
	options.write = cli_markdown_document_write;
	options.user_data = out;
	options.load_image = cli_markdown_document_image;
	options.terminal_fd = -1;
	options.terminal_columns = columns > 0 ? (unsigned int)columns : 1u;
	options.terminal_rows = 24u;
	struct morph_md_kitty *renderer NB_CLEANUP(cli_markdown_renderer_cleanup) =
		morph_md_kitty_create(&options);
	if (!renderer)
		return NB_ERR_NOMEM;
	int rc = morph_md_kitty_append(renderer, normalized, strlen(normalized), 1);
	if (rc == 0)
		rc = morph_md_kitty_render(renderer);
	return rc;
}

struct cli_markdown_retained {
	nb_buf *out;
	int columns, styled, error;
};

static void cli_markdown_retained_preview_cleanup(char **owner)
{
	if (*owner)
		unlink(*owner);
	nb_charp_cleanup(owner);
}

static void cli_markdown_retained_media(const char *type, const char *path, void *opaque)
{
	struct cli_markdown_retained *output = opaque;
	if (output->error || strcmp(type, "video"))
		return;
	nb_buf url NB_CLEANUP(nb_buf_cleanup) = {0};
	output->error = video_file_url_append(path, &url);
	if (output->error)
		return;
	char *preview NB_CLEANUP(cli_markdown_retained_preview_cleanup) = NULL;
	if (output->styled && morph_tui_media_protocol_available() &&
	    !video_preview_create_copy(path, &preview)) {
		nb_buf preview_url NB_CLEANUP(nb_buf_cleanup) = {0};
		nb_buf markdown NB_CLEANUP(nb_buf_cleanup) = {0};
		output->error = video_file_url_append(preview, &preview_url);
		if (!output->error)
			output->error =
				(nb_buf_printf(&markdown, "![](<%s>)\n", nb_buf_cstr(&preview_url))
					 ? 0
					 : NB_ERR_NOMEM);
		if (!output->error)
			output->error =
				cli_markdown_retained_document(output->out, nb_buf_cstr(&markdown),
							       output->columns, output->styled, 0);
	}
	if (!output->error)
		output->error = (nb_buf_printf(output->out,
					       "\n\033]8;;%s\033\\▶ Open video\033]8;;\033\\\n",
					       nb_buf_cstr(&url))
					 ? 0
					 : NB_ERR_NOMEM);
}

static int cli_markdown_retained_document(nb_buf *out, const char *text, int columns, int styled,
					  int image_caption)
{
	if (!out || !text)
		return NB_ERR_INVAL;
	char *normalized NB_CLEANUP(nb_charp_cleanup) = cli_markdown_normalized_copy(text);
	if (!normalized)
		return NB_ERR_NOMEM;
	struct morph_md_kitty_options options;
	cli_markdown_options(&options, 0, 0);
	/* The retained message's View owns its right padding. */
	options.content_padding_right_columns = 0;
	options.write = cli_markdown_document_write;
	options.user_data = out;
	struct cli_markdown_retained output = {.out = out, .columns = columns, .styled = styled};
	options.media = cli_markdown_retained_media;
	options.media_user_data = &output;
	options.terminal_fd = STDOUT_FILENO;
	options.terminal_columns = columns > 0 ? (unsigned)columns : 1u;
	options.image_max_width_columns = CLI_MARKDOWN_IMAGE_MAX_COLUMNS;
	options.image_max_height_rows = CLI_MARKDOWN_IMAGE_MAX_ROWS;
	options.image_caption = image_caption;
	char font[PATH_MAX];
	if (styled && morph_tui_media_protocol_available()) {
		options.load_image = cli_image_loader_load;
		options.release_image = cli_image_loader_release;
		if (cli_markdown_terminal_supports_kitty() &&
		    !morph_data_find(font, sizeof(font), "fonts/STIXTwoMath-Regular.ttf")) {
			options.font_path = font;
			options.features |= MORPH_MD_FEATURE_MATH;
		}
	} else
		options.load_image = cli_markdown_document_image;
	struct morph_md_kitty *renderer NB_CLEANUP(cli_markdown_renderer_cleanup) =
		morph_md_kitty_create(&options);
	if (!renderer)
		return NB_ERR_NOMEM;
	int rc = morph_md_kitty_append(renderer, normalized, strlen(normalized), 1);
	if (!rc)
		rc = morph_md_kitty_render(renderer);
	return rc ? rc : output.error;
}

int cli_markdown_render_retained(nb_buf *out, const char *text, int columns, int styled)
{
	return cli_markdown_retained_document(out, text, columns, styled, 1);
}

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
	if (output && cli_chat_active(output->ctx)) {
		nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
		int rc = (nb_buf_append(&text, bytes, len) ? 0 : NB_ERR_NOMEM);
		if (!rc)
			rc = cli_chat_output(output->ctx, nb_buf_cstr(&text));
		output->prefix_pending = 0;
		cli_output_track_tail(&output->trailing_newlines, bytes, len);
		return rc;
	}
	if (output)
		cli_feed_history_begin(output->ctx);
	if (output && cli_output_result_begin(output->ctx) != 0)
		return NB_ERR_IO;
	/* The renderer buffers incomplete blocks. Emit the prefix only when it
	 * actually writes, so Readline cannot erase a dangling marker meanwhile. */
	if (output && output->prefix_pending) {
		struct morph_tui_view_insets margin = {.top = 1};

		for (int gap = cli_output_block_gap_margin(
			     margin, output->ctx->output.trailing_newlines);
		     gap > 0; gap--)
			cli_output_putchar(output->ctx, '\n');
		cli_output_printf(output->ctx, ANSI_BOLD CLI_FEED_MESSAGE ANSI_RESET " ");
		output->prefix_pending = 0;
	}
	if (output)
		cli_output_track_tail(&output->trailing_newlines, bytes, len);
	captured = cli_output_capture_write(output->ctx, bytes, len);
	if (captured)
		return captured < 0 ? captured : 0;
	if (output->ctx && !cli_turn_details_is_visible(output->ctx)) {
		cli_output_track_tail(&output->ctx->output.trailing_newlines, bytes, len);
		cli_feed_history_advance(output->ctx, bytes, len);
	}
	if (cli_output_color_enabled(output->ctx))
		return fwrite(bytes, 1u, len, stdout) == len ? 0 : NB_ERR_IO;
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
			return NB_ERR_IO;
		i++;
	}
	return 0;
}

static int cli_markdown_preview(const char *bytes, size_t len, void *user)
{
	struct cli_markdown_output *output = user;

	return cli_feed_markdown_preview_set(output->ctx, bytes, len, output->prefix_pending);
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
						  struct cli_markdown_output *output, int preview)
{
	struct morph_md_kitty_options options;
	const char *font_path;

	output->ctx = ctx;
	cli_markdown_options(&options, indent, initial_column);
	font_path = morph_data_find(output->font_path, sizeof(output->font_path),
				    "fonts/STIXTwoMath-Regular.ttf") == 0
			    ? output->font_path
			    : NULL;
	options.font_path = font_path;
	if (enable_math && cli_output_color_enabled(ctx) &&
	    cli_markdown_terminal_supports_kitty() && font_path)
		options.features |= MORPH_MD_FEATURE_MATH;
	options.write = cli_markdown_write;
	options.user_data = output;
	if (preview && cli_feed_can_preview(ctx))
		options.preview = cli_markdown_preview;
	options.media = cb;
	options.media_user_data = user;
	options.load_image = cli_image_loader_load;
	options.release_image = cli_image_loader_release;
	if (cli_chat_active(ctx) &&
	    (!cli_output_color_enabled(ctx) || !morph_tui_media_protocol_available())) {
		options.load_image = cli_markdown_document_image;
		options.release_image = NULL;
	}
	options.image_max_width_columns = CLI_MARKDOWN_IMAGE_MAX_COLUMNS;
	options.image_max_height_rows = CLI_MARKDOWN_IMAGE_MAX_ROWS;
	options.image_caption = 1;
	options.terminal_fd = STDOUT_FILENO;
	return morph_md_kitty_create(&options);
}

static int cli_markdown_render(struct cli_application *ctx, const char *md, unsigned int indent,
			       unsigned int initial_column, cli_markdown_media_cb cb, void *user)
{
	struct cli_markdown_output output = {0};
	struct morph_md_kitty *renderer;
	char *normalized NB_CLEANUP(nb_charp_cleanup) = NULL;
	const char *content;
	int rc;

	if (!md)
		return 0;
	normalized = cli_markdown_normalized_copy(md);
	if (!normalized)
		return 0;
	content = normalized;
	renderer = cli_markdown_create(ctx, indent, initial_column, 1, cb, user, &output, 0);
	if (!renderer) {
		nb_log_write(ctx ? ctx->logger : NULL, NB_LOG_WARN,
			     "failed to initialize Kitty Markdown renderer");
		return output.trailing_newlines;
	}
	rc = morph_md_kitty_append(renderer, content, strlen(content), 1);
	if (rc == 0)
		rc = morph_md_kitty_render(renderer);
	if (rc != 0)
		nb_log_write(ctx ? ctx->logger : NULL, NB_LOG_WARN,
			     "Kitty Markdown rendering failed");
	morph_md_kitty_destroy(renderer);
	fflush(stdout);
	return output.trailing_newlines;
}

void cli_markdown_render_final(struct cli_application *ctx, const char *text)
{
	ctx->presenter->markdown_output.trailing_newlines =
		cli_markdown_render(ctx, text, 2u, 2u, cli_renderer_media, ctx);
}

void cli_markdown_finish_final(struct cli_application *ctx)
{
	while (ctx->presenter->markdown_output.trailing_newlines < 2) {
		cli_output_putchar(ctx, '\n');
		ctx->presenter->markdown_output.trailing_newlines++;
	}
}

int cli_markdown_stream_append(struct cli_application *ctx, const char *delta, int kind)
{
	nb_buf safe NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc;

	if (!ctx || !delta || !delta[0])
		return 0;
	if (ctx->presenter->markdown_stream && ctx->presenter->markdown_stream_kind != kind)
		cli_markdown_stream_reset(ctx, 1);
	if (!ctx->presenter->markdown_stream) {
		ctx->presenter->markdown_output.prefix_pending = 1;
		ctx->presenter->markdown_output.trailing_newlines = 0;
		ctx->presenter->markdown_stream =
			cli_markdown_create(ctx, 2u, 2u, 1, cli_renderer_media, ctx,
					    &ctx->presenter->markdown_output, 1);
		if (!ctx->presenter->markdown_stream)
			return NB_ERR_NOMEM;
		ctx->presenter->markdown_stream_kind = kind;
		ctx->presenter->markdown_stream_visible = 1;
		morph_terminal_sanitizer_init(&ctx->presenter->markdown_stream_sanitizer,
					      MORPH_TERMINAL_TEXT_MULTILINE);
	}
	rc = (nb_buf_init(&safe), nb_buf_reserve(&safe, strlen(delta) + 16) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = morph_terminal_sanitize_feed(&ctx->presenter->markdown_stream_sanitizer, &safe, delta,
					  strlen(delta), 0);
	if (rc == 0 && safe.len > 0)
		rc = (nb_buf_append(&ctx->presenter->markdown_stream_text, nb_buf_cstr(&safe),
				    safe.len)
			      ? 0
			      : NB_ERR_NOMEM);
	if (rc == 0 && safe.len > 0)
		rc = morph_md_kitty_append(ctx->presenter->markdown_stream, nb_buf_cstr(&safe),
					   safe.len, 0);
	nb_buf_cleanup(&safe);
	if (rc != 0)
		return rc;
	return morph_md_kitty_render(ctx->presenter->markdown_stream);
}

int cli_markdown_stream_refresh(struct cli_application *ctx)
{
	return ctx && ctx->presenter->markdown_stream
		       ? morph_md_kitty_render(ctx->presenter->markdown_stream)
		       : 0;
}

void cli_markdown_stream_reset(struct cli_application *ctx, int finish_output)
{
	nb_buf safe NB_CLEANUP(nb_buf_cleanup) = {0};
	int safe_ready = 0;

	if (!ctx)
		return;
	if (ctx->terminal)
		(void)cli_feed_markdown_preview_set(ctx, NULL, 0u, 0);
	if (finish_output && ctx->presenter->markdown_stream) {
		if ((nb_buf_init(&safe), nb_buf_reserve(&safe, 16) ? 0 : NB_ERR_NOMEM) == 0) {
			safe_ready = 1;
			if (morph_terminal_sanitize_feed(&ctx->presenter->markdown_stream_sanitizer,
							 &safe, NULL, 0, 1) == 0 &&
			    safe.len > 0) {
				(void)nb_buf_append(&ctx->presenter->markdown_stream_text,
						    nb_buf_cstr(&safe), safe.len);
				(void)morph_md_kitty_append(ctx->presenter->markdown_stream,
							    nb_buf_cstr(&safe), safe.len, 0);
			}
		}
		if (morph_md_kitty_append(ctx->presenter->markdown_stream, NULL, 0u, 1) == 0)
			(void)morph_md_kitty_render(ctx->presenter->markdown_stream);
	}
	if (safe_ready)
		nb_buf_cleanup(&safe);
	morph_md_kitty_destroy(ctx->presenter->markdown_stream);
	ctx->presenter->markdown_stream = NULL;
	ctx->presenter->markdown_stream_kind = 0;
	ctx->presenter->markdown_stream_visible = 0;
	ctx->presenter->markdown_output.prefix_pending = 0;
	nb_buf_clear(&ctx->presenter->markdown_stream_text);
	morph_terminal_sanitizer_reset(&ctx->presenter->markdown_stream_sanitizer);
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
