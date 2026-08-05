#include "sapi/cli/internal.h"
#include "util/id.h"

#include <fcntl.h>
#include <sys/wait.h>

#define CLIPBOARD_ID_SIZE 64

static int clipboard_run(const char *const argv[], const char *stdout_path)
{
	pid_t pid;
	int output_fd = -1;
	int status;

	if (!argv || !argv[0])
		MORPH_RETURN(-EINVAL);
	if (stdout_path) {
		output_fd = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (output_fd < 0)
			MORPH_RETURN_ERRNO();
	}
	pid = fork();
	if (pid < 0) {
		int rc = -errno;

		if (output_fd >= 0)
			close(output_fd);
		MORPH_RETURN(rc);
	}
	if (pid == 0) {
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
			MORPH_RETURN_ERRNO();
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		MORPH_RETURN(-ENOENT);
	return 0;
}

static int clipboard_output_path(struct cli_context *ctx, char **out_path)
{
	const struct config *config;
	char id[CLIPBOARD_ID_SIZE];
	char *filename;
	char *path;
	int rc;

	if (!ctx || !ctx->runtime || !out_path)
		MORPH_RETURN(-EINVAL);
	*out_path = NULL;
	config = runtime_config_get(ctx->runtime);
	if (!config || !config->general.output_dir[0])
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	rc = file_ensure_dir(config->general.output_dir);
	if (rc != 0)
		MORPH_RETURN(rc);
	rc = morph_random_id("clipboard_", id, sizeof(id));
	if (rc != 0)
		MORPH_RETURN(rc);
	filename = file_path_append_alloc(id, ".png");
	if (!filename)
		MORPH_RETURN(-ENOMEM);
	path = file_path_join_alloc(config->general.output_dir, filename);
	free(filename);
	if (!path)
		MORPH_RETURN(-ENOMEM);
	*out_path = path;
	return 0;
}

#ifdef __APPLE__

static int clipboard_save_platform_image(const char *png_path)
{
	static const char *script =
		"on run argv\n"
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
		MORPH_RETURN(-ENOMEM);
	osascript_argv[0] = "osascript";
	osascript_argv[1] = "-e";
	osascript_argv[2] = script;
	osascript_argv[3] = tiff_path;
	osascript_argv[4] = NULL;
	rc = clipboard_run(osascript_argv, NULL);
	if (rc == 0) {
		sips_argv[0] = "sips";
		sips_argv[1] = "-s";
		sips_argv[2] = "format";
		sips_argv[3] = "png";
		sips_argv[4] = tiff_path;
		sips_argv[5] = "--out";
		sips_argv[6] = png_path;
		sips_argv[7] = NULL;
		rc = clipboard_run(sips_argv, "/dev/null");
	}
	(void)unlink(tiff_path);
	free(tiff_path);
	if (rc != 0)
		MORPH_RETURN(rc);
	return 0;
}

#else

static int clipboard_save_platform_image(const char *png_path)
{
	const char *wl_paste_argv[] = {
		"wl-paste", "--no-newline", "--type", "image/png", NULL
	};
	const char *xclip_argv[] = {
		"xclip", "-selection", "clipboard", "-t", "image/png",
		"-o", NULL
	};
	int rc;

	rc = clipboard_run(wl_paste_argv, png_path);
	if (rc != 0)
		rc = clipboard_run(xclip_argv, png_path);
	if (rc != 0)
		MORPH_RETURN(rc);
	return 0;
}

#endif

int cli_clipboard_save_image(struct cli_context *ctx, char **out_path)
{
	char *path;
	int width;
	int height;
	int channels;
	int rc;

	if (!out_path)
		MORPH_RETURN(-EINVAL);
	*out_path = NULL;
	rc = clipboard_output_path(ctx, &path);
	if (rc != 0)
		MORPH_RETURN(rc);
	rc = clipboard_save_platform_image(path);
	if (rc != 0 || !stbi_info(path, &width, &height, &channels)) {
		(void)unlink(path);
		free(path);
		MORPH_RETURN(rc != 0 ? rc : MORPH_ERR_FORMAT);
	}
	*out_path = path;
	return 0;
}
