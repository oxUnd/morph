#include "image.h"
#include "util/log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *b64_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const unsigned char *in, size_t in_len,
		       char *out, size_t *out_len)
{
	*out_len = 0;
	for (size_t i = 0; i < in_len; i += 3) {
		unsigned int val = (unsigned int)in[i] << 16;
		if (i + 1 < in_len)
			val |= (unsigned int)in[i + 1] << 8;
		if (i + 2 < in_len)
			val |= (unsigned int)in[i + 2];
		out[(*out_len)++] = b64_alphabet[(val >> 18) & 0x3F];
		out[(*out_len)++] = b64_alphabet[(val >> 12) & 0x3F];
		out[(*out_len)++] = (i + 1 < in_len) ? b64_alphabet[(val >> 6) & 0x3F] : '=';
		out[(*out_len)++] = (i + 2 < in_len) ? b64_alphabet[val & 0x3F] : '=';
	}
}

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

static int render_kitty(const char *path)
{
	if (!path || !*path)
		return -1;
	printf("\033_Ga=T,t=f,f=0;%s\033\\\n", path);
	fflush(stdout);
	return 0;
}

static int render_iterm2(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fsize <= 0) { fclose(f); return -1; }
	unsigned char *data = malloc((size_t)fsize);
	if (!data) { fclose(f); return -1; }
	size_t rd = fread(data, 1, (size_t)fsize, f);
	fclose(f);

	size_t b64_len = (rd + 2) / 3 * 4 + 1;
	char *b64 = malloc(b64_len);
	if (!b64) { free(data); return -1; }
	b64_encode(data, rd, b64, &b64_len);
	b64[b64_len] = '\0';
	free(data);

	printf("\033]1337;File=inline=1;size=%zu:", rd);
	printf("%s\a", b64);
	fflush(stdout);
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

	if (detect_kitty()) {
		log_info("image_render: using kitty protocol");
		if (render_kitty(path) == 0) {
			printf("\n");
			return 0;
		}
		log_warn("image_render: kitty protocol failed, trying fallback");
	}
	if (detect_iterm2()) {
		log_info("image_render: using iterm2 protocol");
		if (render_iterm2(path) == 0) {
			printf("\n");
			return 0;
		}
	}
	if (detect_sixel()) {
		log_info("image_render: using sixel");
		if (render_sixel(path) == 0)
			return 0;
	}
	log_info("image_render: no terminal protocol detected, printing path");
	printf("(image: %s)\n", path);
	fflush(stdout);
	return 0;
}