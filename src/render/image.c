#include "image.h"
#include "util/log.h"
#include "util/base64.h"
#include "util/error.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

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

static void write_all(int fd, const char *buf, size_t len)
{
	while (len > 0) {
		ssize_t n = write(fd, buf, len);
		if (n <= 0) break;
		buf += n;
		len -= (size_t)n;
	}
}

static int render_kitty(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		log_warn("render_kitty: cannot open '%s'", path);
		MORPH_RETURN(-EIO);
	}
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fsize <= 0) { fclose(f); MORPH_RETURN(-EIO); }
	unsigned char *data = malloc((size_t)fsize);
	if (!data) { fclose(f); MORPH_RETURN(-ENOMEM); }
	size_t rd = fread(data, 1, (size_t)fsize, f);
	fclose(f);

	int fmt = image_detect_fmt(data, rd);
	int cols = terminal_cols();

	char *b64 = base64_encode(data, rd);
	free(data);
	if (!b64) MORPH_RETURN(-ENOMEM);
	size_t b64_len = strlen(b64);

	log_dbg("render_kitty: path='%s' fmt=%d raw=%zu b64=%zu cols=%d chunks=%zu",
		 path, fmt, rd, b64_len, cols,
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
					"\033_Ga=T,f=%d,c=%d;", fmt, cols);
			else
				hdr_len = snprintf(hdr, sizeof(hdr),
					"\033_Ga=T,f=%d,c=%d,m=1;", fmt, cols);
		} else {
			hdr_len = snprintf(hdr, sizeof(hdr),
				"\033_Gm=%d;", is_last ? 0 : 1);
		}

		write_all(STDOUT_FILENO, hdr, (size_t)hdr_len);
		write_all(STDOUT_FILENO, b64 + i, chunk);
		write_all(STDOUT_FILENO, "\033\\", 2);
	}
	free(b64);

	const char *nl = "\n";
	write_all(STDOUT_FILENO, nl, 1);
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