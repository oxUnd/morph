#include "sapi/cli/internal.h"
#include "sapi/cli/list_ui.h"
#include "util/base64.h"
#include "util/data.h"

#include <sys/ioctl.h>

#define WELCOME_ACCENT "\033[38;5;51m"
#define WELCOME_TEXT "\033[38;5;252m"
#define WELCOME_MUTED "\033[38;5;248m"
#define WELCOME_PADDING 2

/* Small, inline brand asset; unsupported terminals keep the text wordmark. */
static int welcome_logo(void)
{
	const char *term = getenv("TERM");
	const char *program = getenv("TERM_PROGRAM");
	int kitty = getenv("KITTY_WINDOW_ID") != NULL ||
		(term && strstr(term, "kitty")) ||
		(program && strcmp(program, "ghostty") == 0);
	int iterm = getenv("ITERM_PROFILE") != NULL ||
		(program && strcmp(program, "iTerm.app") == 0);
	char path[PATH_MAX];
	size_t length = 0;
	char *data;
	char *encoded;
	morph_buf_t control;
	int rc;

	if (!cli_color_enabled() || !isatty(STDOUT_FILENO) ||
	    getenv("TMUX") || getenv("STY") || (!kitty && !iterm))
		return 0;
	if (morph_data_find(path, sizeof(path), "morph-logo.png") != 0)
		return 0;
	data = file_read_all(path, &length);
	if (!data)
		return 0;
	encoded = base64_encode((const unsigned char *)data, length);
	free(data);
	if (!encoded)
		return 0;
	/* The bundled 36px PNG fits a single Kitty graphics packet. */
	if (strlen(encoded) > 4096 || morph_buf_init(&control, 4096) != 0) {
		free(encoded);
		return 0;
	}
	if (kitty) {
		rc = morph_buf_printf(&control,
			"\033_Ga=T,f=100,c=2,r=1,C=1,q=2;%s\033\\\033[2C", encoded);
	} else {
		rc = morph_buf_printf(&control,
			"\0337\033]1337;File=inline=1;size=%zu;width=2;height=1;"
			"preserveAspectRatio=1:%s\a\0338\033[2C", length, encoded);
	}
	free(encoded);
	if (rc == 0) {
		fflush(stdout);
		size_t written = fwrite(control.data, 1, control.len, stdout);

		rc = written == control.len ? 0 : -EIO;
	}
	morph_buf_cleanup(&control);
	return rc == 0;
}

static void welcome_value(const char *value, size_t width, int keep_tail)
{
	char clipped[BUFSIZ];
	char *safe = utf8_terminal_sanitize_dup(value, strlen(value),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);

	if (!safe)
		return;
	(void)utf8_copy_ellipsized_display_width(clipped, sizeof(clipped), safe,
		width, keep_tail);
	printf("%s", clipped);
	free(safe);
}

struct welcome_hint {
	const char *key;
	const char *description;
};

static void welcome_hints(const struct welcome_hint *hints, size_t count,
	size_t width, const char *color)
{
	size_t used = 0;

	printf("  ");
	for (size_t i = 0; i < count; i++) {
		size_t length = utf8_display_width(hints[i].key) + 1 +
			utf8_display_width(hints[i].description);

		if (used && used + 3 + length > width) {
			printf("\n  ");
			used = 0;
		}
		if (used) {
			printf(WELCOME_MUTED " · " ANSI_RESET);
			used += 3;
		}
		printf("%s", color);
		welcome_value(hints[i].key, width - used, 0);
		printf(ANSI_RESET WELCOME_MUTED " ");
		size_t key_width = utf8_display_width(hints[i].key) + 1;

		if (key_width < width - used)
			welcome_value(hints[i].description, width - used - key_width, 0);
		printf(ANSI_RESET);
		used += length;
	}
	putchar('\n');
}

void cli_print_welcome(const char *model, const char *directory)
{
	static const struct welcome_hint commands[] = {
		{ "/help", "commands" },
		{ "/switch", "sessions" },
		{ "./image.png", "attach" },
	};
	static const struct welcome_hint keys[] = {
		{ "Enter", "send/adjust" },
		{ "Ctrl+J", "newline" },
		{ "Esc", "cancel" },
		{ "Ctrl+Cmd+V", "image" },
	};
	struct winsize size = {0};
	int columns = cli_list_columns();
	size_t width;
	size_t model_width;
	size_t path_width;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col)
		columns = size.ws_col;
	width = columns > 2 * WELCOME_PADDING ?
		(size_t)(columns - 2 * WELCOME_PADDING) : 1;
	model = model && model[0] ? model : "model";
	directory = directory && directory[0] ? directory : ".";
	printf("\n  ");
	if (columns >= 30 && welcome_logo())
		putchar(' ');
	printf(WELCOME_TEXT ANSI_BOLD "morph" ANSI_RESET WELCOME_MUTED "  v%s"
		ANSI_RESET "\n  " WELCOME_TEXT, MORPH_VERSION);
	model_width = utf8_display_width(model);
	path_width = utf8_display_width(directory);
	if (model_width + 3 + path_width > width) {
		model_width = width / 3;
		if (model_width > 24)
			model_width = 24;
	}
	welcome_value(model, model_width, 0);
	if (width > model_width + 3) {
		printf(ANSI_RESET WELCOME_MUTED " · " ANSI_RESET WELCOME_TEXT);
		welcome_value(directory, width - model_width - 3, 1);
	}
	printf(ANSI_RESET "\n\n");
	welcome_hints(commands, sizeof(commands) / sizeof(commands[0]), width,
		WELCOME_ACCENT);
	welcome_hints(keys, sizeof(keys) / sizeof(keys[0]), width, WELCOME_TEXT ANSI_BOLD);
	putchar('\n');
}
