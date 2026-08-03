#include "image.h"
#include "morph_kitty_protocol.h"
#include "util/buf.h"
#include "util/log.h"
#include "util/base64.h"
#include "util/error.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*
 * stb_image / stb_image_write are already compiled with their
 * STB_*_IMPLEMENTATION macros in src/util/image_util.c (linked via
 * morph-util). Including the headers here without the impl macros
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

static int terminal_cols(void)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return (int)ws.ws_col;
	return 80;
}

static int terminal_rows(void)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
		return (int)ws.ws_row;
	return 24;
}

static void write_all(int fd, const char *buf, size_t len)
{
	while (len > 0) {
		ssize_t n = write(fd, buf, len);
		if (n <= 0) break;
		buf += n;
		len -= (size_t)n;
	}
}

static int kitty_stdout_write(const char *bytes, size_t len, void *user_data)
{
	size_t offset = 0u;

	(void)user_data;
	while (offset < len) {
		ssize_t count = write(
			STDOUT_FILENO, bytes + offset, len - offset);

		if (count < 0)
			return -errno;
		if (count == 0)
			return -EIO;
		offset += (size_t)count;
	}
	return 0;
}

/* stb_image_write callback: append bytes into a growing buffer. */
struct png_sink {
	morph_buf_t buf;
	int oom;
};

static void png_sink_write(void *ctx, void *data, int size)
{
	struct png_sink *s = ctx;
	if (s->oom || size <= 0) return;
	if (morph_buf_append(&s->buf, data, (size_t)size) != 0)
		s->oom = 1;
}

/*
 * Read file at path into a freshly malloc'd buffer. Returns 0 on success
 * and stores buf+len. On error returns negative errno; caller does not
 * need to free.
 */
static int read_file_all(const char *path, unsigned char **out, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -EIO;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -EIO; }
	long fsize = ftell(f);
	if (fsize <= 0) { fclose(f); return -EIO; }
	if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -EIO; }
	unsigned char *data = malloc((size_t)fsize);
	if (!data) { fclose(f); return -ENOMEM; }
	size_t rd = fread(data, 1, (size_t)fsize, f);
	fclose(f);
	if (rd != (size_t)fsize) { free(data); return -EIO; }
	*out = data;
	*out_len = rd;
	return 0;
}

/*
 * Kitty's graphics protocol only accepts f=24 (RGB), f=32 (RGBA),
 * and f=100 (PNG). JPEG/WebP/GIF/BMP are NOT directly supported.
 * If the input is not already PNG, decode it with stb_image and
 * re-encode as PNG before transmitting.
 *
 * On success: *out_data / *out_len is a malloc'd buffer the caller
 * must free; *out_fmt is set to 100.
 */
static int load_as_png(const char *path, unsigned char **out_data,
		       size_t *out_len, int *out_fmt)
{
	unsigned char *raw = NULL;
	size_t raw_len = 0;
	int rc = read_file_all(path, &raw, &raw_len);
	if (rc != 0) return rc;

	int fmt = image_detect_fmt(raw, raw_len);
	if (fmt == 100) {
		*out_data = raw;
		*out_len = raw_len;
		*out_fmt = 100;
		return 0;
	}

	/* Not PNG (JPEG, or unknown like WebP/GIF/BMP). Decode + reencode. */
	int w = 0, h = 0, ch = 0;
	unsigned char *pixels = stbi_load_from_memory(raw, (int)raw_len,
						      &w, &h, &ch, 4);
	free(raw);
	if (!pixels) {
		log_warn("render_kitty: stbi_load_from_memory failed for '%s': %s",
			 path, stbi_failure_reason());
		return -EIO;
	}

	struct png_sink sink;
	sink.oom = 0;
	if (morph_buf_init(&sink.buf, 4096) != 0) {
		stbi_image_free(pixels);
		MORPH_RETURN(-ENOMEM);
	}
	int wrote = stbi_write_png_to_func(png_sink_write, &sink,
					   w, h, 4, pixels, w * 4);
	stbi_image_free(pixels);
	if (!wrote || sink.oom || sink.buf.len == 0) {
		morph_buf_cleanup(&sink.buf);
		MORPH_RETURN(-ENOMEM);
	}

	*out_len = sink.buf.len;
	*out_data = (unsigned char *)morph_buf_detach(&sink.buf);
	*out_fmt = 100;
	return 0;
}

static int render_kitty(const char *path)
{
	unsigned char *data = NULL;
	size_t rd = 0;
	int fmt = 0;
	int rc = load_as_png(path, &data, &rd, &fmt);
	if (rc != 0) {
		log_warn("render_kitty: load_as_png('%s') failed: %d", path, rc);
		MORPH_RETURN(rc);
	}

	int cols = terminal_cols();
	int rows_limit = terminal_rows();
	int pixel_width;
	int pixel_height;
	int channels;
	int disp_rows;
	double placement_rows;
	double cell_width = 8.0;
	double cell_height = 16.0;
	struct winsize ws;
	/*
	 * 默认按终端宽度的 50% 显示，避免大图占满整个屏幕。
	 * kitty 在只指定 c= 时会自动保持宽高比，按列数缩放。
	 */
	int disp_cols = cols / 2;
	if (disp_cols < 1) disp_cols = 1;
	if (disp_cols > (int)MORPH_KITTY_PLACEHOLDER_LIMIT)
		disp_cols = (int)MORPH_KITTY_PLACEHOLDER_LIMIT;
	if (rd > (size_t)INT_MAX ||
	    !stbi_info_from_memory(
		    data, (int)rd, &pixel_width, &pixel_height, &channels) ||
	    pixel_width <= 0 || pixel_height <= 0) {
		free(data);
		MORPH_RETURN(-EINVAL);
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
	placement_rows = (double)pixel_height * disp_cols * cell_width /
		((double)pixel_width * cell_height);
	if (placement_rows > rows_limit) {
		double scaled_columns = (double)disp_cols * rows_limit /
			placement_rows;

		disp_cols = (int)(scaled_columns + 0.999999);
		if (disp_cols < 1)
			disp_cols = 1;
		disp_rows = rows_limit;
	} else {
		disp_rows = (int)(placement_rows + 0.999999);
		if (disp_rows < 1)
			disp_rows = 1;
	}

	char *b64 = base64_encode(data, rd);
	free(data);
	if (!b64) MORPH_RETURN(-ENOMEM);
	size_t b64_len = strlen(b64);

	uint32_t image_id = morph_kitty_image_id_new();
	log_dbg("render_kitty: path='%s' fmt=%d raw=%zu b64=%zu "
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
					fmt, image_id, disp_cols,
					disp_rows);
			else
				hdr_len = snprintf(hdr, sizeof(hdr),
					"\033_Ga=T,f=%d,i=%u,U=1,q=2,"
					"c=%d,r=%d,m=1;",
					fmt, image_id, disp_cols,
					disp_rows);
		} else {
			hdr_len = snprintf(hdr, sizeof(hdr),
				"\033_Gm=%d;", is_last ? 0 : 1);
		}

		write_all(STDOUT_FILENO, hdr, (size_t)hdr_len);
		write_all(STDOUT_FILENO, b64 + i, chunk);
		write_all(STDOUT_FILENO, "\033\\", 2);
	}
	free(b64);

	for (int row = 0; row < disp_rows; row++) {
		rc = morph_kitty_write_placeholder_row(
			kitty_stdout_write, NULL, image_id,
			(unsigned int)row, (unsigned int)disp_cols);
		if (rc != 0)
			MORPH_RETURN(rc);
		write_all(STDOUT_FILENO, "\n", 1);
	}
	return 0;
}

static int render_iterm2(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		MORPH_RETURN(-EIO);
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fsize <= 0) { fclose(f); MORPH_RETURN(-EIO); }
	unsigned char *data = malloc((size_t)fsize);
	if (!data) { fclose(f); MORPH_RETURN(-ENOMEM); }
	size_t rd = fread(data, 1, (size_t)fsize, f);
	fclose(f);

	char *b64 = base64_encode(data, rd);
	free(data);
	if (!b64) MORPH_RETURN(-ENOMEM);
	size_t b64_len = strlen(b64);

	fflush(stdout);
	char hdr[128];
	int hdr_len = snprintf(hdr, sizeof(hdr),
		"\033]1337;File=inline=1;size=%zu:", rd);
	write_all(STDOUT_FILENO, hdr, (size_t)hdr_len);
	write_all(STDOUT_FILENO, b64, b64_len);
	write_all(STDOUT_FILENO, "\a", 1);
	free(b64);
	return 0;
}

static int render_sixel(const char *path)
{
	(void)path;
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "chafa -f sixel \"%s\" 2>/dev/null || "
		 "img2sixel \"%s\" 2>/dev/null || echo \"(sixel: need chafa or img2sixel)\"", path, path);
	fflush(stdout);
	return system(cmd);
}

int image_terminal_protocol_available(void)
{
	return detect_kitty() || detect_iterm2() || detect_sixel();
}

int image_render_terminal(const char *path)
{
	if (!path || !*path) {
		log_warn("image_render: no path provided");
		return -EINVAL;
	}

	log_dbg("image_render: path='%s' kitty=%d iterm2=%d sixel=%d",
		 path, detect_kitty(), detect_iterm2(), detect_sixel());

	if (detect_kitty()) {
		log_dbg("image_render: trying kitty protocol");
		if (render_kitty(path) == 0)
			return 0;
		log_warn("image_render: kitty protocol failed, trying fallback");
	}
	if (detect_iterm2()) {
		log_dbg("image_render: trying iterm2 protocol");
		if (render_iterm2(path) == 0) {
			write_all(STDOUT_FILENO, "\n", 1);
			return 0;
		}
	}
	if (detect_sixel()) {
		log_dbg("image_render: trying sixel");
		if (render_sixel(path) == 0)
			return 0;
	}
	log_dbg("image_render: no terminal protocol detected, printing path");
	printf("(image: %s)\n", path);
	fflush(stdout);
	return 0;
}
