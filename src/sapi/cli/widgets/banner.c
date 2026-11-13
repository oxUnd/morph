#include "sapi/cli/widgets/banner.h"
#include "nb/err.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/widgets/list.h"
#include "nb/encoding.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/cleanup.h"
#include "runtime/resources.h"
#include "nb/file.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
#include <morph/tui/widgets/image.h>
#include <morph/tui/widget.h>
#include <morph/tui/widgets/text_view.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLI_BANNER_TEXT "\033[38;5;252m"
#define CLI_BANNER_MUTED "\033[38;5;245m"
#define CLI_BANNER_RULE "\033[38;5;238m"
#define CLI_BANNER_PADDING 2u
#define CLI_BANNER_MAX_WIDTH 76u
#define CLI_BANNER_SPLIT_WIDTH 72u
#define CLI_BANNER_ACTION_WIDTH 27u
#define CLI_BANNER_COLUMN_GAP 4u
#define CLI_BANNER_LABEL_WIDTH 11u

/* Small, inline brand asset; unsupported terminals keep the text wordmark. */
static int cli_banner_logo(struct cli_application *ctx)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_1 NB_CLEANUP(nb_charp_cleanup) = NULL;

	const char *term = getenv("TERM");
	const char *program = getenv("TERM_PROGRAM");
	int kitty = getenv("KITTY_WINDOW_ID") != NULL || (term && strstr(term, "kitty")) ||
		    (program && strcmp(program, "ghostty") == 0);
	int iterm =
		getenv("ITERM_PROFILE") != NULL || (program && strcmp(program, "iTerm.app") == 0);
	char path[PATH_MAX];
	size_t length = 0;
	char *data NB_CLEANUP(nb_charp_cleanup) = NULL;
	nb_buf encoded_bytes NB_CLEANUP(nb_buf_destroy) = {0};
	const char *encoded;
	nb_buf control NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc;

	if (!cli_output_color_enabled(ctx) || !isatty(STDOUT_FILENO) ||
	    (ctx && ctx->output.capture) || cli_output_is_structured(ctx) ||
	    getenv("TMUX") || getenv("STY") || (!kitty && !iterm))
		return 0;
	if (morph_data_find(path, sizeof(path), "morph-logo.png") != 0)
		return 0;
	data = (nb_file_read_all(path, &file_value_1, 0, NULL) == NB_OK &&
				(file_copy_1 = (char *)malloc(nb_buf_len(&file_value_1) + 1)) !=
					NULL
			? (memcpy(file_copy_1, nb_buf_cstr(&file_value_1),
				  nb_buf_len(&file_value_1) + 1),
			   *(&length) = nb_buf_len(&file_value_1), nb_charp_steal(&file_copy_1))
			: NULL);
	if (!data)
		return 0;
	if (nb_base64_encode((const unsigned char *)data, length, NB_B64_STD, true, &encoded_bytes,
			     NULL) != NB_OK)
		return 0;
	encoded = nb_buf_cstr(&encoded_bytes);
	/* The bundled 36px PNG fits a single Kitty graphics packet. */
	if (strlen(encoded) > 4096 ||
	    (nb_buf_init(&control), nb_buf_reserve(&control, 4096) ? 0 : NB_ERR_NOMEM) != 0)
		return 0;
	if (kitty) {
		rc = (nb_buf_printf(&control, "\033_Ga=T,f=100,c=2,r=1,C=1,q=2;%s\033\\\033[2C",
				    encoded)
			      ? 0
			      : NB_ERR_NOMEM);
	} else {
		rc = (nb_buf_printf(&control,
				    "\0337\033]1337;File=inline=1;size=%zu;width=2;height=1;"
				    "preserveAspectRatio=1:%s\a\0338\033[2C",
				    length, encoded)
			      ? 0
			      : NB_ERR_NOMEM);
	}
	if (rc == 0) {
		fflush(stdout);
		size_t written = fwrite(control.ptr, 1, control.len, stdout);

		rc = written == control.len ? 0 : NB_ERR_IO;
	}
	return rc == 0;
}

static void cli_banner_spaces(struct cli_application *ctx, size_t count)
{
	cli_output_printf(ctx, "%*s", (int)count, "");
}

/* Metadata has already been sanitized; clipping always uses terminal cells. */
static size_t cli_banner_text(struct cli_application *ctx, const char *text,
			     size_t width, int keep_tail)
{
	char clipped[BUFSIZ];

	(void)morph_terminal_copy_ellipsized_display_width(clipped, sizeof(clipped), text, width,
							   keep_tail);
	cli_output_printf(ctx, "%s", clipped);
	return nb_utf8_width_cstr(clipped);
}

static void cli_banner_header(struct cli_application *ctx, size_t width, size_t padding,
	int retained_logo)
{
	const char *version = "v" MORPH_VERSION;
	size_t version_width = nb_utf8_width_cstr(version);
	size_t used = 0;

	cli_banner_spaces(ctx, padding);
	if (width >= 30 && (retained_logo || cli_banner_logo(ctx))) {
		if (retained_logo)
			cli_banner_spaces(ctx, 3);
		else
			cli_output_putchar(ctx, ' ');
		used = 3;
	}
	cli_output_printf(ctx, CLI_COMMAND_COLOR ANSI_BOLD);
	used += cli_banner_text(ctx, "morph", width - used, 0);
	cli_output_printf(ctx, ANSI_RESET);
	if (width >= used + version_width + 2) {
		cli_banner_spaces(ctx, width - used - version_width);
		cli_output_printf(ctx, CLI_BANNER_MUTED "%s" ANSI_RESET, version);
	}
	cli_output_putchar(ctx, '\n');
	cli_banner_spaces(ctx, padding);
	cli_output_printf(ctx, CLI_BANNER_RULE);
	for (size_t column = 0; column < width; column++)
		cli_output_printf(ctx, "─");
	cli_output_printf(ctx, ANSI_RESET "\n");
}

static size_t cli_banner_field(struct cli_application *ctx, const char *label,
			      const char *value, size_t width, int keep_tail)
{
	size_t used = 0;

	if (width >= CLI_BANNER_LABEL_WIDTH + 7u) {
		cli_output_printf(ctx, CLI_BANNER_MUTED "%s" ANSI_RESET, label);
		cli_banner_spaces(ctx, CLI_BANNER_LABEL_WIDTH - nb_utf8_width_cstr(label));
		used = CLI_BANNER_LABEL_WIDTH;
	}
	cli_output_printf(ctx, CLI_BANNER_TEXT);
	used += cli_banner_text(ctx, value, width - used, keep_tail);
	cli_output_printf(ctx, ANSI_RESET);
	return used;
}

struct cli_banner_hint {
	const char *key;
	const char *description;
};

static size_t cli_banner_hint_print(struct cli_application *ctx,
				  const struct cli_banner_hint *hint, size_t width,
				  size_t description_column, const char *key_color)
{
	cli_output_printf(ctx, "%s", key_color);
	size_t used = cli_banner_text(ctx, hint->key, width, 0);

	cli_output_printf(ctx, ANSI_RESET);
	size_t gap = description_column > used ? description_column - used : 1;

	if (width - used <= gap)
		return used;
	cli_banner_spaces(ctx, gap);
	cli_output_printf(ctx, CLI_BANNER_MUTED);
	used += gap + cli_banner_text(ctx, hint->description, width - used - gap, 0);
	cli_output_printf(ctx, ANSI_RESET);
	return used;
}

static void cli_banner_hints(struct cli_application *ctx,
			     const struct cli_banner_hint *hints, size_t count,
			     size_t width, size_t padding, const char *key_color)
{
	size_t used = 0;

	cli_banner_spaces(ctx, padding);
	for (size_t i = 0; i < count; i++) {
		size_t length = nb_utf8_width_cstr(hints[i].key) + 1 +
				nb_utf8_width_cstr(hints[i].description);

		if (used && used + 3 + length > width) {
			cli_output_putchar(ctx, '\n');
			cli_banner_spaces(ctx, padding);
			used = 0;
		}
		if (used) {
			cli_banner_spaces(ctx, 3);
			used += 3;
		}
		used += cli_banner_hint_print(ctx, &hints[i], width - used, 0, key_color);
	}
	cli_output_putchar(ctx, '\n');
}

static void cli_banner_format(struct cli_application *ctx, const char *model,
			      const char *directory, size_t width, size_t padding,
			      int retained_logo)
{
	static const struct cli_banner_hint commands[] = {
		{"/help", "Commands"},
		{"/switch", "Sessions"},
		{"./image.png", "Attach image"},
	};
	static const struct cli_banner_hint keys[] = {
		{"Enter", "Send"},
		{"Ctrl+J", "New line"},
		{"Esc", "Cancel"},
		{"Ctrl+Cmd+V", "Paste image"},
	};
	if (cli_output_is_structured(ctx))
		return;
	if (width > CLI_BANNER_MAX_WIDTH)
		width = CLI_BANNER_MAX_WIDTH;
	model = model && model[0] ? model : "model";
	directory = directory && directory[0] ? directory : ".";
	char *safe_model NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		model, strlen(model), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	char *safe_directory NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		directory, strlen(directory), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);

	if (!safe_model || !safe_directory)
		return;
	cli_output_putchar(ctx, '\n');
	cli_banner_header(ctx, width, padding, retained_logo);
	if (width >= CLI_BANNER_SPLIT_WIDTH) {
		size_t left = width - CLI_BANNER_ACTION_WIDTH - CLI_BANNER_COLUMN_GAP;

		for (size_t row = 0; row < sizeof(commands) / sizeof(commands[0]); row++) {
			size_t used = 0;

			cli_banner_spaces(ctx, padding);
			if (row == 0)
				used = cli_banner_field(ctx, "Workspace", safe_directory, left, 1);
			else if (row == 1)
				used = cli_banner_field(ctx, "Model", safe_model, left, 0);
			cli_banner_spaces(ctx, left - used + CLI_BANNER_COLUMN_GAP);
			(void)cli_banner_hint_print(ctx, &commands[row], CLI_BANNER_ACTION_WIDTH,
						   13, CLI_COMMAND_COLOR);
			cli_output_putchar(ctx, '\n');
		}
	} else {
		cli_banner_spaces(ctx, padding);
		(void)cli_banner_field(ctx, "Workspace", safe_directory, width, 1);
		cli_output_putchar(ctx, '\n');
		cli_banner_spaces(ctx, padding);
		(void)cli_banner_field(ctx, "Model", safe_model, width, 0);
		cli_output_printf(ctx, "\n\n");
		cli_banner_hints(ctx, commands, sizeof(commands) / sizeof(commands[0]), width,
				 padding, CLI_COMMAND_COLOR);
	}
	cli_output_putchar(ctx, '\n');
	cli_banner_hints(ctx, keys, sizeof(keys) / sizeof(keys[0]), width, padding,
			 CLI_BANNER_MUTED);
	cli_output_putchar(ctx, '\n');
}

void cli_banner_print(struct cli_application *ctx, const char *model, const char *directory)
{
	int columns = cli_list_columns();
	size_t padding = columns > (int)(2u * CLI_BANNER_PADDING) ? CLI_BANNER_PADDING : 0;
	size_t width = columns > 0 ? (size_t)columns - 2u * padding : 1;
	cli_banner_format(ctx, model, directory, width, padding, 0);
}

struct cli_banner {
	struct morph_tui_widget base;
	struct cli_application *ctx;
	struct morph_tui_view *text, *logo;
	int columns;
};

static int cli_banner_measure(struct morph_tui_view *view, int width,
			      struct morph_tui_view_size *size)
{
	struct cli_banner *banner = (struct cli_banner *)view;
	if (banner->columns != width) {
		nb_buf rendered NB_CLEANUP(nb_buf_cleanup) = {0};
		nb_buf directory NB_CLEANUP(nb_buf_cleanup) = {0};
		const char *workdir = runtime_workdir_borrow(banner->ctx->runtime);
		const char *home = getenv("HOME");
		if (!workdir || !*workdir)
			workdir = ".";
		int rc = 0;
		if (home && *home && !strncmp(workdir, home, strlen(home)) &&
		    (workdir[strlen(home)] == '/' || workdir[strlen(home)] == '\0')) {
			rc = (nb_buf_append_char(&directory, '~') ? 0 : NB_ERR_NOMEM);
			workdir += strlen(home);
		}
		if (!rc)
			rc = (nb_buf_append_cstr(&directory, workdir) ? 0 : NB_ERR_NOMEM);
		if (!rc)
			rc = cli_output_capture_styled_begin(banner->ctx, &rendered);
		if (rc)
			return rc;
		int logo = banner->logo && width >= 30;
		if (banner->logo)
			morph_tui_view_set_hidden(banner->logo, !logo);
		cli_banner_format(banner->ctx,
				  runtime_config_borrow(banner->ctx->runtime)->models.text.model,
				  nb_buf_cstr(&directory), width > 0 ? (size_t)width : 1u, 0, logo);
		cli_output_capture_end(banner->ctx);
		if (nb_buf_status(&rendered))
			return NB_ERR_NOMEM;
		rc = cli_messages_set_rendered_view(banner->text, nb_buf_cstr(&rendered),
						    cli_output_color_enabled(banner->ctx));
		if (rc)
			return rc;
		banner->columns = width;
	}
	return morph_tui_view_measure(banner->text, width, size);
}

static int cli_banner_layout(struct morph_tui_view *view)
{
	struct cli_banner *banner = (struct cli_banner *)view;
	struct morph_tui_view_rect rect = morph_tui_view_content_rect(view);
	int rc = morph_tui_view_arrange(banner->text, rect);
	if (!rc && banner->logo && !morph_tui_view_hidden(banner->logo))
		rc = morph_tui_view_arrange(banner->logo,
			(struct morph_tui_view_rect){rect.x, rect.y, 2, 1});
	return rc;
}

static void cli_banner_asset_cleanup(struct morph_tui_media_asset **owner)
{
	morph_tui_media_asset_destroy(*owner);
	*owner = NULL;
}

static struct morph_tui_view *cli_banner_logo_add_borrow(struct morph_tui_builder *builder,
	struct morph_tui_view *parent, struct cli_application *ctx)
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_2 NB_CLEANUP(nb_charp_cleanup) = NULL;

	const char *term = getenv("TERM");
	const char *program = getenv("TERM_PROGRAM");
	int kitty = getenv("KITTY_WINDOW_ID") || (term && strstr(term, "kitty")) ||
		    (program && !strcmp(program, "ghostty"));
	int iterm = getenv("ITERM_PROFILE") || (program && !strcmp(program, "iTerm.app"));
	if (!cli_output_color_enabled(ctx) || getenv("TMUX") || getenv("STY") ||
	    (!kitty && !iterm))
		return NULL;
	char path[PATH_MAX];
	if (morph_data_find(path, sizeof(path), "morph-logo.png"))
		return NULL;
	size_t length = 0;
	char *bytes NB_CLEANUP(nb_charp_cleanup) =
		(nb_file_read_all(path, &file_value_2, 0, NULL) == NB_OK &&
				 (file_copy_2 = (char *)malloc(nb_buf_len(&file_value_2) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_2, nb_buf_cstr(&file_value_2),
				   nb_buf_len(&file_value_2) + 1),
			    *(&length) = nb_buf_len(&file_value_2), nb_charp_steal(&file_copy_2))
			 : NULL);
	if (!bytes)
		return NULL;
	struct morph_tui_media_asset *asset NB_CLEANUP(cli_banner_asset_cleanup) = NULL;
	if (morph_tui_media_asset_create_png_copy(&asset, (const unsigned char *)bytes, length))
		return NULL;
	const struct morph_tui_view_options options = {.width = 2, .height = 1};
	return morph_tui_widget_image_add_borrow(builder, parent, asset, &options);
}

struct morph_tui_view *cli_banner_add_borrow(struct morph_tui_builder *builder,
	struct morph_tui_view *parent, struct cli_application *ctx)
{
	static const struct morph_tui_view_ops cli_banner_ops = {
		.measure = cli_banner_measure, .layout = cli_banner_layout};
	const struct morph_tui_view_options options = {
		.margin = {.top = 1},
		.padding = {.left = CLI_BANNER_PADDING, .right = CLI_BANNER_PADDING}};
	struct cli_banner *banner = (struct cli_banner *)morph_tui_widget_add_borrow(
		builder, parent, sizeof(*banner), &cli_banner_ops, &options);
	if (!banner)
		return NULL;
	banner->ctx = ctx;
	banner->columns = -1;
	banner->logo = cli_banner_logo_add_borrow(builder, &banner->base.view, ctx);
	banner->text = morph_tui_widget_text_view_add_borrow(builder, &banner->base.view, "", NULL);
	if (banner->text) {
		morph_tui_view_set_focusable(banner->text, 0);
		morph_tui_builder_fail(builder,
			morph_tui_widget_text_view_set_content_sizing(banner->text, 1));
	}
	return &banner->base.view;
}
