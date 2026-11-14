#include "sapi/cli/adapters/clipboard/clipboard.h"
#include "nb/time.h"
#include <string.h>
#include <stdlib.h>
#include "nb/cleanup.h"
#include "nb/err.h"
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "config/config.h"
#include "stb_image.h"
#include "nb/process.h"
#include "sapi/cli/application_internal.h"
#include "nb/random.h"
#include "nb/encoding.h"
#include "sapi/cli/widgets/feed.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>

#define CLI_CLIPBOARD_ID_SIZE 64
#define CLI_CLIPBOARD_COPY_TIMEOUT_MS 5000
#define CLI_CLIPBOARD_OSC52_MAX_BYTES 100000

static int cli_clipboard_write_text(const char *const argv[], const char *text, int64_t deadline)
{
	nb_process_options options = {
		.argv = argv,
		.search_path = true,
		.close_fds = true,
		.new_group = true,
		.stdout_mode = NB_PROCESS_NULL,
		.stderr_mode = NB_PROCESS_NULL,
	};
	nb_process *child NB_CLEANUP(nb_process_cleanup) = NULL;
	int rc = nb_process_spawn(&options, &child, NULL);
	if (rc != NB_OK)
		return rc;
	int64_t remaining = deadline - nb_time_monotonic_ms();
	nb_process_exchange_options exchange = {
		.timeout_ms = remaining > 0 ? (int)remaining : 1,
		.max_output = 0,
	};
	nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf errors NB_CLEANUP(nb_buf_cleanup) = {0};
	int code = 0;
	rc = nb_process_exchange(child, text, strlen(text), &exchange, &output, &errors, &code,
				 NULL);
	if (rc == NB_OK && code != 0)
		return code == 127 ? NB_ERR_NOT_FOUND : NB_ERR_IO;
	return rc;
}

static int cli_clipboard_native_text(const char *text)
{
	int64_t deadline = nb_time_monotonic_ms() + CLI_CLIPBOARD_COPY_TIMEOUT_MS;
#ifdef __APPLE__
	const char *argv[] = {"pbcopy", NULL};

	return cli_clipboard_write_text(argv, text, deadline);
#else
	const char *wl_copy[] = {"wl-copy", "--type", "text/plain;charset=utf-8", NULL};
	const char *xclip[] = {"xclip", "-selection", "clipboard", "-t", "UTF8_STRING", "-i", NULL};
	const char *xsel[] = {"xsel", "--clipboard", "--input", NULL};
	const char *const *backends[] = {wl_copy, xclip, xsel};
	int rc = NB_ERR_NOT_FOUND;

	for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
		rc = cli_clipboard_write_text(backends[i], text, deadline);
		if (!rc || nb_time_monotonic_ms() >= deadline)
			break;
	}
	return rc;
#endif
}

static int cli_clipboard_environment(const char *name)
{
	const char *value = getenv(name);

	return value && value[0];
}

static int cli_clipboard_terminal_text(struct cli_application *ctx, const char *text)
{
	nb_buf sequence NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf encoded_bytes NB_CLEANUP(nb_buf_destroy) = {0};
	const char *encoded;
	int tmux = cli_clipboard_environment("TMUX");
	int rc;

	if (!ctx || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE || !isatty(STDOUT_FILENO))
		return NB_ERR_UNSUPPORTED;
	if (strlen(text) > CLI_CLIPBOARD_OSC52_MAX_BYTES)
		return NB_ERR_TOO_BIG;
	if (nb_base64_encode((const unsigned char *)text, strlen(text), NB_B64_STD, true,
			     &encoded_bytes, NULL) != NB_OK)
		return NB_ERR_NOMEM;
	encoded = nb_buf_cstr(&encoded_bytes);
	rc = (nb_buf_init(&sequence),
	      nb_buf_reserve(&sequence, strlen(encoded) + 32) ? 0 : NB_ERR_NOMEM);
	if (!rc) {
		rc = (nb_buf_printf(&sequence,
				    tmux ? "\033Ptmux;\033\033]52;c;%s\007\033\\"
					 : "\033]52;c;%s\007",
				    encoded)
			      ? 0
			      : NB_ERR_NOMEM);
		if (!rc) {
			cli_feed_history_begin(ctx);
			if (fwrite(sequence.ptr, 1, sequence.len, stdout) != sequence.len ||
			    fflush(stdout) != 0)
				rc = errno ? nb_status_from_errno(errno, NULL) : NB_ERR_IO;
		}
		nb_buf_cleanup(&sequence);
	}
	return rc;
}

int cli_clipboard_copy_text(struct cli_application *ctx, const char *text)
{
	if (!ctx || !text)
		return NB_ERR_INVAL;
	if (!text[0])
		return NB_ERR_NO_DATA;
	int native = cli_clipboard_native_text(text);
	int terminal = NB_ERR_UNSUPPORTED;

	/* Forward for remote clients even when the host clipboard succeeds. */
	if (native || cli_clipboard_environment("TMUX") ||
	    cli_clipboard_environment("SSH_CONNECTION") || cli_clipboard_environment("SSH_TTY") ||
	    cli_clipboard_environment("SSH_CLIENT"))
		terminal = cli_clipboard_terminal_text(ctx, text);
	if (!native)
		return CLI_CLIPBOARD_COPIED;
	if (!terminal)
		return CLI_CLIPBOARD_REQUESTED;
	return terminal == NB_ERR_UNSUPPORTED ? native : terminal;
}

static int cli_clipboard_run(const char *const argv[], const char *stdout_path)
{
	if (!argv || !argv[0])
		return NB_ERR_INVAL;
	int output_fd = -1;
	if (stdout_path) {
		output_fd = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		if (output_fd < 0)
			return nb_status_from_errno(errno, NULL);
	}
	nb_process_options options = {
		.argv = argv,
		.search_path = true,
		.close_fds = true,
		.stdin_mode = NB_PROCESS_INHERIT,
		.stdout_mode = stdout_path ? NB_PROCESS_FD : NB_PROCESS_INHERIT,
		.stdout_fd = output_fd,
		.stderr_mode = NB_PROCESS_NULL,
	};
	nb_process *child NB_CLEANUP(nb_process_cleanup) = NULL;
	int rc = nb_process_spawn(&options, &child, NULL);
	if (output_fd >= 0)
		close(output_fd);
	if (rc != NB_OK)
		return rc;
	int code = 0;
	rc = nb_process_wait(child, -1, &code, NULL, NULL);
	return rc != NB_OK ? rc : code == 0 ? NB_OK : NB_ERR_NOT_FOUND;
}

static int cli_clipboard_output_path(struct cli_application *ctx, char **out_path)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};

	const struct config *config;
	char id[CLI_CLIPBOARD_ID_SIZE];
	char *filename;
	char *path;
	int rc;

	if (!ctx || !ctx->runtime || !out_path)
		return NB_ERR_INVAL;
	*out_path = NULL;
	config = runtime_config_borrow(ctx->runtime);
	if (!config || !config->general.output_dir[0])
		return MORPH_ERR_NOT_CONFIGURED;
	rc = nb_file_mkdir(config->general.output_dir, true, NULL);
	if (rc != 0)
		return rc;
	rc = ((file_status_1 = ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
				: !(nb_buf_clear(&file_value_1),
				    nb_buf_append_cstr(&file_value_1, "clipboard_"))
					? NB_ERR_NOMEM
					: nb_random_hex(&file_value_1, 16, NULL))) != NB_OK
		      ? file_status_1
	      : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), id, sizeof(id))) ==
			      NB_ERR_RANGE
		      ? NB_ERR_NO_SPACE
		      : file_status_1);
	if (rc != 0)
		return rc;
	filename = (nb_buf_clear(&file_value_2), nb_buf_printf(&file_value_2, "%s%s", id, ".png")
							 ? strdup(nb_buf_cstr(&file_value_2))
							 : NULL);
	if (!filename)
		return NB_ERR_NOMEM;
	path = (nb_path_concat(&file_value_3, config->general.output_dir, filename) == NB_OK
			? strdup(nb_buf_cstr(&file_value_3))
			: NULL);
	free(filename);
	if (!path)
		return NB_ERR_NOMEM;
	*out_path = path;
	return 0;
}

#ifdef __APPLE__

static int cli_clipboard_save_platform_image(const char *png_path)
{
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};

	static const char script[] = "on run argv\n"
				     "set outFile to open for access POSIX file (item 1 of argv) "
				     "with write permission\n"
				     "try\n"
				     "set eof outFile to 0\n"
				     "write (the clipboard as «class TIFF») to outFile\n"
				     "close access outFile\n"
				     "on error messageText number errorNumber\n"
				     "try\nclose access outFile\nend try\n"
				     "error messageText number errorNumber\n"
				     "end try\n"
				     "end run";
	char *tiff_path;
	const char *osascript_argv[5];
	const char *sips_argv[8];
	int rc;

	tiff_path = (nb_buf_clear(&file_value_4),
		     nb_buf_printf(&file_value_4, "%s%s", png_path, ".tiff")
			     ? strdup(nb_buf_cstr(&file_value_4))
			     : NULL);
	if (!tiff_path)
		return NB_ERR_NOMEM;
	osascript_argv[0] = "osascript";
	osascript_argv[1] = "-e";
	osascript_argv[2] = script;
	osascript_argv[3] = tiff_path;
	osascript_argv[4] = NULL;
	rc = cli_clipboard_run(osascript_argv, NULL);
	if (rc == 0) {
		sips_argv[0] = "sips";
		sips_argv[1] = "-s";
		sips_argv[2] = "format";
		sips_argv[3] = "png";
		sips_argv[4] = tiff_path;
		sips_argv[5] = "--out";
		sips_argv[6] = png_path;
		sips_argv[7] = NULL;
		rc = cli_clipboard_run(sips_argv, "/dev/null");
	}
	(void)unlink(tiff_path);
	free(tiff_path);
	if (rc != 0)
		return rc;
	return 0;
}

#else

static int cli_clipboard_save_platform_image(const char *png_path)
{
	const char *wl_paste_argv[] = {"wl-paste", "--no-newline", "--type", "image/png", NULL};
	const char *xclip_argv[] = {"xclip",	 "-selection", "clipboard", "-t",
				    "image/png", "-o",	       NULL};
	int rc;

	rc = cli_clipboard_run(wl_paste_argv, png_path);
	if (rc != 0)
		rc = cli_clipboard_run(xclip_argv, png_path);
	if (rc != 0)
		return rc;
	return 0;
}

#endif

int cli_clipboard_save_image(struct cli_application *ctx, char **out_path)
{
	char *path;
	int width;
	int height;
	int channels;
	int rc;

	if (!out_path)
		return NB_ERR_INVAL;
	if (*out_path)
		return NB_ERR_EXISTS;
	rc = cli_clipboard_output_path(ctx, &path);
	if (rc != 0)
		return rc;
	rc = cli_clipboard_save_platform_image(path);
	if (rc != 0 || !stbi_info(path, &width, &height, &channels)) {
		(void)unlink(path);
		free(path);
		return rc != 0 ? rc : NB_ERR_FORMAT;
	}
	*out_path = path;
	return 0;
}
