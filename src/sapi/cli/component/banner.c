#include "sapi/cli/component/banner.h"
#include <nb/err.h>
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/component/report.h"
#include "sapi/cli/component/list.h"
#include <nb/encoding.h>
#include <nb/buf.h>
#include "sapi/cli/adapters/media/asset.h"
#include <nb/cleanup.h>
#include "runtime/resources.h"
#include <nb/file.h>
#include <nb/utf8.h>
#include <nothing/tui/terminal/text.h>
#include <nothing/tui/views/image_view.h>
#include <nothing/tui/view.h>
#include <nothing/tui/views/text_view.h>

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
	nb_buf data NB_CLEANUP(nb_buf_cleanup) = {0};

	const char *term = getenv("TERM");
	const char *program = getenv("TERM_PROGRAM");
	int kitty = getenv("KITTY_WINDOW_ID") != NULL || (term && strstr(term, "kitty")) ||
		    (program && strcmp(program, "ghostty") == 0);
	int iterm =
		getenv("ITERM_PROFILE") != NULL || (program && strcmp(program, "iTerm.app") == 0);
	char path[PATH_MAX];
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
	if (nb_file_read_all(path, &data, 0, NULL) != NB_OK)
		return 0;
	size_t length = nb_buf_len(&data);
	if (nb_base64_encode((const unsigned char *)nb_buf_cstr(&data), length, NB_B64_STD, true,
			     &encoded_bytes, NULL) != NB_OK)
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

	(void)nt_terminal_copy_ellipsized_display_width(clipped, sizeof(clipped), text, width,
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
	char *safe_model NB_CLEANUP(nb_charp_cleanup) =
		nt_sanitize_dup(model, strlen(model), NT_TEXT_SINGLE_LINE, NULL);
	char *safe_directory NB_CLEANUP(nb_charp_cleanup) =
		nt_sanitize_dup(directory, strlen(directory), NT_TEXT_SINGLE_LINE, NULL);

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
	struct cli_report report;
	struct cli_application *ctx;
	struct nt_view *logo;
};

static int cli_banner_format_report(struct cli_report *report, nb_buf *rendered, int width)
{
	struct cli_banner *banner = (struct cli_banner *)report;
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
		rc = cli_output_capture_styled_begin(banner->ctx, rendered);
	if (rc)
		return rc;
	/* The banner keeps its logical rows even when both gutters consume the
	 * viewport. Its text child clips those rows to the visible content. */
	const int gutters = (int)(2u * CLI_BANNER_PADDING);
	width = width > gutters ? width - gutters : 1;
	int logo = banner->logo && width >= 30;
	cli_banner_format(banner->ctx,
			  runtime_config_borrow(banner->ctx->runtime)->models.text.model,
			  nb_buf_cstr(&directory), (size_t)width, 0, logo);
	cli_output_capture_end(banner->ctx);
	return nb_buf_status(rendered);
}

static int cli_banner_layout(struct nt_view *view)
{
	struct cli_banner *banner = (struct cli_banner *)view;
	int rc = cli_report_class.layout(view);
	struct nt_view_rect rect = nt_view_content_rect(banner->report.text);
	if (!rc && banner->logo)
		rc = nt_view_set_hidden(banner->logo, rect.width < 30);
	if (!rc && banner->logo && !nt_view_is_hidden(banner->logo))
		rc = nt_view_arrange(banner->logo, (struct nt_view_rect){rect.x, rect.y, 2, 1});
	return rc;
}

static void cli_banner_asset_cleanup(struct nt_media_asset **owner)
{
	nt_media_asset_release(*owner);
	*owner = NULL;
}

static struct nt_view *cli_banner_logo_add_borrow(struct nt_builder *builder,
	struct nt_view *parent, struct cli_application *ctx)
{
	nb_buf bytes NB_CLEANUP(nb_buf_destroy) = {0};

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
	if (nb_file_read_all(path, &bytes, 0, NULL) != NB_OK)
		return NULL;
	struct nt_media_asset *asset NB_CLEANUP(cli_banner_asset_cleanup) = NULL;
	if (cli_media_asset_from_png(&asset, (const unsigned char *)nb_buf_cstr(&bytes), bytes.len))
		return NULL;
	const struct nt_view_options options = {.width = 2, .height = 1};
	return nt_image_view_add(builder, parent, asset, &options);
}

struct nt_view *cli_banner_add_borrow(struct nt_builder *builder,
	struct nt_view *parent, struct cli_application *ctx)
{
	static const struct nt_view_class cli_banner_class = {
		.superclass = &cli_report_class,
		.instance_size = sizeof(struct cli_banner),
		.kind = "cli_banner",
		.layout = cli_banner_layout,
	};
	const struct nt_view_options options = {.width = NT_FILL, .margin = {.top = 1}};
	struct cli_banner *banner = (struct cli_banner *)cli_report_add(
		builder, parent, &cli_banner_class, &options, cli_output_color_enabled(ctx),
		cli_banner_format_report);
	if (!banner)
		return NULL;
	const struct nt_view_options gutters = {
		.padding = {.left = CLI_BANNER_PADDING, .right = CLI_BANNER_PADDING}};
	nt_builder_fail(builder, nt_view_set_options(banner->report.text, &gutters));
	banner->ctx = ctx;
	banner->logo = cli_banner_logo_add_borrow(builder, &banner->report.base, ctx);
	return &banner->report.base;
}
