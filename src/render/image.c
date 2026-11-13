#include "image.h"
#include <morph/tui/terminal/size.h>
#include "morph_kitty_protocol.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/log.h"
#include "nb/encoding.h"
#include "errors.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*
 * stb_image / stb_image_write are already compiled with their
 * STB_*_IMPLEMENTATION macros in src/media/image.c (linked via
 * morph-media). Including the headers here without the impl macros
 * just gives us the prototypes; the symbols resolve at link time.
 */
#include "stb_image.h"
#include "stb_image_write.h"

static int detect_kitty(void)
{
	if (getenv("KITTY_WINDOW_ID"))
		return 1;
	const char *term = getenv("TERM");
	return term && strstr(term, "kitty");
}

static int detect_iterm2(void)
{
	return getenv("ITERM_PROFILE") != NULL;
}

static int detect_sixel(void)
{
	const char *term = getenv("TERM");
	return term && strstr(term, "sixel");
}

int image_detect_fmt(const unsigned char *data, size_t len)
{
	if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
		return 100;
	if (len >= 2 && data[0] == 0xFF && data[1] == 0xD8)
		return 101;
	return 0;
}

static int kitty_stdout_write(const char *bytes, size_t len, void *user_data)
{
	(void)user_data;
	return nb_fd_write_until(STDOUT_FILENO, bytes, len, INT64_MAX, NULL, NULL, NULL);
}

/* stb_image_write appends binary bytes into a native buffer owner. */
static void png_sink_write(void *context, void *data, int size)
{
	if (size > 0)
		(void)nb_buf_append(context, data, (size_t)size);
}

/* Kitty accepts PNG/RGB/RGBA, so other source formats are decoded and encoded
 * as PNG. The caller retains a native binary owner throughout transmission. */
static int load_as_png(struct nb_log *logger, const char *path, nb_buf *output, int *format)
{
	nb_buf source NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf png NB_CLEANUP(nb_buf_destroy) = {0};
	int rc = nb_file_read_all(path, &source, 0, NULL);
	if (rc != NB_OK)
		return rc;
	if (image_detect_fmt((const unsigned char *)nb_buf_cstr(&source), nb_buf_len(&source)) ==
	    100) {
		nb_buf_move(output, &source);
		*format = 100;
		return NB_OK;
	}
	if (nb_buf_len(&source) > INT_MAX)
		return NB_ERR_TOO_BIG;
	int width = 0;
	int height = 0;
	int channels = 0;
	unsigned char *pixels =
		stbi_load_from_memory((const unsigned char *)nb_buf_cstr(&source),
				      (int)nb_buf_len(&source), &width, &height, &channels, 4);
	if (!pixels) {
		nb_log_write(logger, NB_LOG_WARN, "cannot decode terminal image '%s': %s", path,
			     stbi_failure_reason());
		return NB_ERR_IO;
	}
	int wrote =
		stbi_write_png_to_func(png_sink_write, &png, width, height, 4, pixels, width * 4);
	stbi_image_free(pixels);
	if (!wrote || !nb_buf_ok(&png) || nb_buf_empty(&png))
		return NB_ERR_NOMEM;
	nb_buf_move(output, &png);
	*format = 100;
	return NB_OK;
}

static int render_kitty(struct nb_log *logger, const char *path)
{
	nb_buf pixels NB_CLEANUP(nb_buf_destroy) = {0};
	int fmt = 0;
	int rc = load_as_png(logger, path, &pixels, &fmt);
	if (rc != 0) {
		nb_log_write(logger, NB_LOG_WARN, "render_kitty: load_as_png('%s') failed: %s",
			     path, nb_status_str(rc));
		return rc;
	}

	const unsigned char *data = (const unsigned char *)nb_buf_cstr(&pixels);
	size_t rd = nb_buf_len(&pixels);

	int cols;
	int rows_limit;

	morph_terminal_dimensions(STDOUT_FILENO, &cols, &rows_limit);
	int pixel_width;
	int pixel_height;
	int channels;
	int disp_rows;
	double placement_rows;
	double cell_width = 8.0;
	double cell_height = 16.0;
	struct winsize ws;
	/*
	 * Use half the terminal width while preserving the source aspect ratio.
	 * Kitty scales by columns when only the c parameter is specified.
	 */
	int disp_cols = cols / 2;
	if (disp_cols < 1)
		disp_cols = 1;
	if (disp_cols > (int)MORPH_KITTY_PLACEHOLDER_LIMIT)
		disp_cols = (int)MORPH_KITTY_PLACEHOLDER_LIMIT;
	if (rd > (size_t)INT_MAX ||
	    !stbi_info_from_memory(data, (int)rd, &pixel_width, &pixel_height, &channels) ||
	    pixel_width <= 0 || pixel_height <= 0) {
		return NB_ERR_INVAL;
	}
	memset(&ws, 0, sizeof(ws));
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
		if (ws.ws_col > 0 && ws.ws_xpixel > 0)
			cell_width = (double)ws.ws_xpixel / ws.ws_col;
		if (ws.ws_row > 0 && ws.ws_ypixel > 0)
			cell_height = (double)ws.ws_ypixel / ws.ws_row;
	}
	if (rows_limit > (int)MORPH_KITTY_PLACEHOLDER_LIMIT)
		rows_limit = (int)MORPH_KITTY_PLACEHOLDER_LIMIT;
	placement_rows =
		(double)pixel_height * disp_cols * cell_width / ((double)pixel_width * cell_height);
	if (placement_rows > rows_limit) {
		double scaled_columns = (double)disp_cols * rows_limit / placement_rows;

		disp_cols = (int)(scaled_columns + 0.999999);
		if (disp_cols < 1)
			disp_cols = 1;
		disp_rows = rows_limit;
	} else {
		disp_rows = (int)(placement_rows + 0.999999);
		if (disp_rows < 1)
			disp_rows = 1;
	}

	nb_buf encoded NB_CLEANUP(nb_buf_destroy) = {0};
	nb_status status = nb_base64_encode(data, rd, NB_B64_STD, true, &encoded, NULL);
	if (status != NB_OK)
		return NB_ERR_NOMEM;
	const char *b64 = nb_buf_cstr(&encoded);
	size_t b64_len = nb_buf_len(&encoded);

	uint32_t image_id = morph_kitty_image_id_new();
	nb_log_write(logger, NB_LOG_DEBUG,
		     "render_kitty: path='%s' fmt=%d raw=%zu b64=%zu "
		     "cols=%d placement=%dx%d id=%u chunks=%zu",
		     path, fmt, rd, b64_len, cols, disp_cols, disp_rows, image_id,
		     (b64_len + 4095) / 4096);

	fflush(stdout);

	const size_t chunk_size = 4096;
	for (size_t i = 0; i < b64_len; i += chunk_size) {
		size_t chunk = (b64_len - i < chunk_size) ? (b64_len - i) : chunk_size;
		int is_last = (i + chunk >= b64_len);

		char hdr[128];
		int hdr_len;
		if (i == 0) {
			if (is_last)
				hdr_len = snprintf(hdr, sizeof(hdr),
						   "\033_Ga=T,f=%d,i=%u,U=1,q=2,"
						   "c=%d,r=%d;",
						   fmt, image_id, disp_cols, disp_rows);
			else
				hdr_len = snprintf(hdr, sizeof(hdr),
						   "\033_Ga=T,f=%d,i=%u,U=1,q=2,"
						   "c=%d,r=%d,m=1;",
						   fmt, image_id, disp_cols, disp_rows);
		} else {
			hdr_len = snprintf(hdr, sizeof(hdr), "\033_Gm=%d;", is_last ? 0 : 1);
		}

		(void)nb_fd_write_until(STDOUT_FILENO, hdr, (size_t)hdr_len, INT64_MAX, NULL, NULL,
					NULL);
		(void)nb_fd_write_until(STDOUT_FILENO, b64 + i, chunk, INT64_MAX, NULL, NULL, NULL);
		(void)nb_fd_write_until(STDOUT_FILENO, "\033\\", 2, INT64_MAX, NULL, NULL, NULL);
	}

	for (int row = 0; row < disp_rows; row++) {
		rc = morph_kitty_write_placeholder_row(kitty_stdout_write, NULL, image_id,
						       (unsigned int)row, (unsigned int)disp_cols);
		if (rc != 0)
			return rc;
		(void)nb_fd_write_until(STDOUT_FILENO, "\n", 1, INT64_MAX, NULL, NULL, NULL);
	}
	return 0;
}

static int render_iterm2(const char *path)
{
	nb_buf source NB_CLEANUP(nb_buf_destroy) = {0};
	int rc = nb_file_read_all(path, &source, 0, NULL);
	if (rc != NB_OK)
		return rc;
	if (nb_buf_empty(&source))
		return NB_ERR_IO;
	const unsigned char *data = (const unsigned char *)nb_buf_cstr(&source);
	size_t rd = nb_buf_len(&source);

	nb_buf encoded NB_CLEANUP(nb_buf_destroy) = {0};
	nb_status status = nb_base64_encode(data, rd, NB_B64_STD, true, &encoded, NULL);
	if (status != NB_OK)
		return NB_ERR_NOMEM;
	const char *b64 = nb_buf_cstr(&encoded);
	size_t b64_len = nb_buf_len(&encoded);

	fflush(stdout);
	char hdr[128];
	int hdr_len = snprintf(hdr, sizeof(hdr), "\033]1337;File=inline=1;size=%zu:", rd);
	(void)nb_fd_write_until(STDOUT_FILENO, hdr, (size_t)hdr_len, INT64_MAX, NULL, NULL, NULL);
	(void)nb_fd_write_until(STDOUT_FILENO, b64, b64_len, INT64_MAX, NULL, NULL, NULL);
	(void)nb_fd_write_until(STDOUT_FILENO, "\a", 1, INT64_MAX, NULL, NULL, NULL);
	return 0;
}

static int render_sixel(const char *path)
{
	(void)path;
	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		 "chafa -f sixel \"%s\" 2>/dev/null || "
		 "img2sixel \"%s\" 2>/dev/null || echo \"(sixel: need chafa or img2sixel)\"",
		 path, path);
	fflush(stdout);
	return system(cmd);
}

int image_terminal_protocol_available(void)
{
	return detect_kitty() || detect_iterm2() || detect_sixel();
}

int image_render_terminal(struct nb_log *logger, const char *path)
{
	if (!path || !*path) {
		nb_log_write(logger, NB_LOG_WARN, "image_render: no path provided");
		return NB_ERR_INVAL;
	}

	nb_log_write(logger, NB_LOG_DEBUG, "image_render: path='%s' kitty=%d iterm2=%d sixel=%d",
		     path, detect_kitty(), detect_iterm2(), detect_sixel());

	if (detect_kitty()) {
		nb_log_write(logger, NB_LOG_DEBUG, "image_render: trying kitty protocol");
		if (render_kitty(logger, path) == 0)
			return 0;
		nb_log_write(logger, NB_LOG_WARN,
			     "image_render: kitty protocol failed, trying fallback");
	}
	if (detect_iterm2()) {
		nb_log_write(logger, NB_LOG_DEBUG, "image_render: trying iterm2 protocol");
		if (render_iterm2(path) == 0) {
			(void)nb_fd_write_until(STDOUT_FILENO, "\n", 1, INT64_MAX, NULL, NULL,
						NULL);
			return 0;
		}
	}
	if (detect_sixel()) {
		nb_log_write(logger, NB_LOG_DEBUG, "image_render: trying sixel");
		if (render_sixel(path) == 0)
			return 0;
	}
	nb_log_write(logger, NB_LOG_DEBUG,
		     "image_render: no terminal protocol detected, printing path");
	printf("(image: %s)\n", path);
	fflush(stdout);
	return 0;
}
