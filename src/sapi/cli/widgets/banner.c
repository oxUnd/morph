#include "sapi/cli/widgets/banner.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/widgets/list.h"
#include "util/base64.h"
#include "util/buf.h"
#include "util/cleanup.h"
#include "util/data.h"
#include "util/file.h"
#include "util/utf8.h"

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
	const char *term = getenv("TERM");
	const char *program = getenv("TERM_PROGRAM");
	int kitty = getenv("KITTY_WINDOW_ID") != NULL || (term && strstr(term, "kitty")) ||
		    (program && strcmp(program, "ghostty") == 0);
	int iterm =
		getenv("ITERM_PROFILE") != NULL || (program && strcmp(program, "iTerm.app") == 0);
	char path[PATH_MAX];
	size_t length = 0;
	char *data MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	char *encoded MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	morph_buf_t control MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc;

	if (!cli_output_color_enabled(ctx) || !isatty(STDOUT_FILENO) ||
	    (ctx && ctx->output.capture) || cli_output_is_structured(ctx) ||
	    getenv("TMUX") || getenv("STY") || (!kitty && !iterm))
		return 0;
	if (morph_data_find(path, sizeof(path), "morph-logo.png") != 0)
		return 0;
	data = file_read_all_copy(path, &length);
	if (!data)
		return 0;
	encoded = base64_encode_copy((const unsigned char *)data, length);
	if (!encoded)
		return 0;
	/* The bundled 36px PNG fits a single Kitty graphics packet. */
	if (strlen(encoded) > 4096 || morph_buf_init(&control, 4096) != 0)
		return 0;
	if (kitty) {
		rc = morph_buf_printf(&control, "\033_Ga=T,f=100,c=2,r=1,C=1,q=2;%s\033\\\033[2C",
				      encoded);
	} else {
		rc = morph_buf_printf(&control,
				      "\0337\033]1337;File=inline=1;size=%zu;width=2;height=1;"
				      "preserveAspectRatio=1:%s\a\0338\033[2C",
				      length, encoded);
	}
	if (rc == 0) {
		fflush(stdout);
		size_t written = fwrite(control.data, 1, control.len, stdout);

		rc = written == control.len ? 0 : -EIO;
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

	(void)utf8_copy_ellipsized_display_width(clipped, sizeof(clipped), text, width,
					       keep_tail);
	cli_output_printf(ctx, "%s", clipped);
	return utf8_display_width(clipped);
}

static void cli_banner_header(struct cli_application *ctx, size_t width, size_t padding)
{
	const char *version = "v" MORPH_VERSION;
	size_t version_width = utf8_display_width(version);
	size_t used = 0;

	cli_banner_spaces(ctx, padding);
	if (width >= 30 && cli_banner_logo(ctx)) {
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
		cli_banner_spaces(ctx, CLI_BANNER_LABEL_WIDTH - utf8_display_width(label));
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
		size_t length = utf8_display_width(hints[i].key) + 1 +
				utf8_display_width(hints[i].description);

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

void cli_banner_print(struct cli_application *ctx, const char *model, const char *directory)
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
	int columns = cli_list_columns();
	size_t padding = columns > (int)(2u * CLI_BANNER_PADDING) ? CLI_BANNER_PADDING : 0;
	size_t width = columns > 0 ? (size_t)columns - 2u * padding : 1;

	if (cli_output_is_structured(ctx))
		return;
	if (width > CLI_BANNER_MAX_WIDTH)
		width = CLI_BANNER_MAX_WIDTH;
	model = model && model[0] ? model : "model";
	directory = directory && directory[0] ? directory : ".";
	char *safe_model MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
		model, strlen(model), UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	char *safe_directory MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
		directory, strlen(directory), UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);

	if (!safe_model || !safe_directory)
		return;
	cli_output_putchar(ctx, '\n');
	cli_banner_header(ctx, width, padding);
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
