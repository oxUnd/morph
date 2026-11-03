#include "sapi/cli/adapters/clipboard/clipboard.h"
#include "config/config.h"
#include "stb_image.h"
#include "util/signals.h"
#include "sapi/cli/application_internal.h"
#include "util/id.h"
#include "util/base64.h"
#include "util/child_io.h"
#include "sapi/cli/widgets/feed.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>

#define CLI_CLIPBOARD_ID_SIZE 64
#define CLI_CLIPBOARD_COPY_TIMEOUT_MS 5000
#define CLI_CLIPBOARD_OSC52_MAX_BYTES 100000

static int cli_clipboard_spawn_text(const char *const argv[], const int input[2], int null_fd,
				    pid_t *pid)
{
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	extern char **environ;
	int rc = posix_spawn_file_actions_init(&actions);

	if (rc)
		return -rc;
	rc = posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
	if (!rc)
		rc = posix_spawn_file_actions_adddup2(&actions, null_fd, STDOUT_FILENO);
	if (!rc)
		rc = posix_spawn_file_actions_adddup2(&actions, null_fd, STDERR_FILENO);
	for (int i = 0; i < 2 && !rc; i++)
		rc = posix_spawn_file_actions_addclose(&actions, input[i]);
	if (!rc)
		rc = posix_spawn_file_actions_addclose(&actions, null_fd);
	if (!rc) {
		rc = posix_spawnattr_init(&attributes);
		if (!rc) {
			sigset_t empty;
			short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP;

			sigemptyset(&empty);
			rc = posix_spawnattr_setsigmask(&attributes, &empty);
			if (!rc)
				rc = posix_spawnattr_setpgroup(&attributes, 0);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
			flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
			if (!rc)
				rc = posix_spawnattr_setflags(&attributes, flags);
			if (!rc)
				rc = posix_spawnp(pid, argv[0], &actions, &attributes,
						  (char *const *)argv, environ);
			posix_spawnattr_destroy(&attributes);
		}
	}
	posix_spawn_file_actions_destroy(&actions);
	return -rc;
}

static int cli_clipboard_write_text(const char *const argv[], const char *text, int64_t deadline)
{
	morph_buf_t output = {0};
	morph_buf_t errors = {0};
	int input[2] = {-1, -1};
	int null_fd = -1;
	int status;
	pid_t pid;
	int rc = morph_buf_init(&output, 16);

	if (!rc)
		rc = morph_buf_init(&errors, 16);
	if (!rc)
		rc = morph_io_pipe(input);
	if (!rc) {
		null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
		if (null_fd < 0)
			rc = -errno;
	}
	if (!rc)
		rc = cli_clipboard_spawn_text(argv, input, null_fd, &pid);
	if (input[0] >= 0)
		close(input[0]);
	if (!rc) {
		int64_t remaining = deadline - morph_io_now_ms();

		/* An EOF-only output fd avoids waiting for a clipboard daemon's stdout. */
		rc = morph_child_exchange(pid, input[1], null_fd, -1, text, strlen(text),
			remaining > 0 ? (int)remaining : 1, 0, NULL, NULL,
			&output, &errors, &status);
		input[1] = -1;
		if ((!rc || rc == -EPIPE) &&
		    (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
			rc = -EIO;
	}
	if (input[1] >= 0)
		close(input[1]);
	if (null_fd >= 0)
		close(null_fd);
	morph_buf_cleanup(&errors);
	morph_buf_cleanup(&output);
	return rc;
}

static int cli_clipboard_native_text(const char *text)
{
	int64_t deadline = morph_io_now_ms() + CLI_CLIPBOARD_COPY_TIMEOUT_MS;
#ifdef __APPLE__
	const char *argv[] = {"pbcopy", NULL};

	return cli_clipboard_write_text(argv, text, deadline);
#else
	const char *wl_copy[] = {"wl-copy", "--type", "text/plain;charset=utf-8", NULL};
	const char *xclip[] = {"xclip", "-selection", "clipboard", "-t", "UTF8_STRING", "-i", NULL};
	const char *xsel[] = {"xsel", "--clipboard", "--input", NULL};
	const char *const *backends[] = {wl_copy, xclip, xsel};
	int rc = -ENOENT;

	for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
		rc = cli_clipboard_write_text(backends[i], text, deadline);
		if (!rc || morph_io_now_ms() >= deadline)
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
	morph_buf_t sequence;
	char *encoded;
	int tmux = cli_clipboard_environment("TMUX");
	int rc;

	if (!ctx || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE || !isatty(STDOUT_FILENO))
		return -ENOTSUP;
	if (strlen(text) > CLI_CLIPBOARD_OSC52_MAX_BYTES)
		return -E2BIG;
	encoded = base64_encode((const unsigned char *)text, strlen(text));
	if (!encoded)
		return -ENOMEM;
	rc = morph_buf_init(&sequence, strlen(encoded) + 32);
	if (!rc) {
		rc = morph_buf_printf(&sequence, tmux ? "\033Ptmux;\033\033]52;c;%s\007\033\\" :
			"\033]52;c;%s\007", encoded);
		if (!rc) {
			cli_feed_history_begin(ctx);
			if (fwrite(sequence.data, 1, sequence.len, stdout) != sequence.len ||
			    fflush(stdout) != 0)
				rc = errno ? -errno : -EIO;
		}
		morph_buf_cleanup(&sequence);
	}
	free(encoded);
	return rc;
}

int cli_clipboard_copy_text(struct cli_application *ctx, const char *text)
{
	if (!ctx || !text)
		return -EINVAL;
	if (!text[0])
		return -ENODATA;
	int native = cli_clipboard_native_text(text);
	int terminal = -ENOTSUP;

	/* Forward for remote clients even when the host clipboard succeeds. */
	if (native || cli_clipboard_environment("TMUX") ||
	    cli_clipboard_environment("SSH_CONNECTION") || cli_clipboard_environment("SSH_TTY") ||
	    cli_clipboard_environment("SSH_CLIENT"))
		terminal = cli_clipboard_terminal_text(ctx, text);
	if (!native)
		return CLI_CLIPBOARD_COPIED;
	if (!terminal)
		return CLI_CLIPBOARD_REQUESTED;
	return terminal == -ENOTSUP ? native : terminal;
}

static int cli_clipboard_run(const char *const argv[], const char *stdout_path)
{
	pid_t pid;
	int output_fd = -1;
	int status;

	if (!argv || !argv[0])
		return -EINVAL;
	if (stdout_path) {
		output_fd = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (output_fd < 0)
			return morph_error_from_errno(errno);
	}
	pid = fork();
	if (pid < 0) {
		int rc = -errno;

		if (output_fd >= 0)
			close(output_fd);
		return rc;
	}
	if (pid == 0) {
		morph_signals_reset_child();
		int null_fd = open("/dev/null", O_WRONLY);

		if (output_fd >= 0 && dup2(output_fd, STDOUT_FILENO) < 0)
			_exit(126);
		if (null_fd >= 0 && dup2(null_fd, STDERR_FILENO) < 0)
			_exit(126);
		if (output_fd >= 0)
			close(output_fd);
		if (null_fd >= 0)
			close(null_fd);
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	if (output_fd >= 0)
		close(output_fd);
	for (;;) {
		pid_t waited = waitpid(pid, &status, 0);

		if (waited >= 0)
			break;
		if (errno != EINTR)
			return morph_error_from_errno(errno);
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -ENOENT;
	return 0;
}

static int cli_clipboard_output_path(struct cli_application *ctx, char **out_path)
{
	const struct config *config;
	char id[CLI_CLIPBOARD_ID_SIZE];
	char *filename;
	char *path;
	int rc;

	if (!ctx || !ctx->runtime || !out_path)
		return -EINVAL;
	*out_path = NULL;
	config = runtime_config_get(ctx->runtime);
	if (!config || !config->general.output_dir[0])
		return MORPH_ERR_NOT_CONFIGURED;
	rc = file_ensure_dir(config->general.output_dir);
	if (rc != 0)
		return rc;
	rc = morph_random_id("clipboard_", id, sizeof(id));
	if (rc != 0)
		return rc;
	filename = file_path_append_alloc(id, ".png");
	if (!filename)
		return -ENOMEM;
	path = file_path_join_alloc(config->general.output_dir, filename);
	free(filename);
	if (!path)
		return -ENOMEM;
	*out_path = path;
	return 0;
}

#ifdef __APPLE__

static int cli_clipboard_save_platform_image(const char *png_path)
{
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

	tiff_path = file_path_append_alloc(png_path, ".tiff");
	if (!tiff_path)
		return -ENOMEM;
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
		return -EINVAL;
	*out_path = NULL;
	rc = cli_clipboard_output_path(ctx, &path);
	if (rc != 0)
		return rc;
	rc = cli_clipboard_save_platform_image(path);
	if (rc != 0 || !stbi_info(path, &width, &height, &channels)) {
		(void)unlink(path);
		free(path);
		return rc != 0 ? rc : MORPH_ERR_FORMAT;
	}
	*out_path = path;
	return 0;
}
