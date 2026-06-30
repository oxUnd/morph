#include "id.h"
#include "error.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef __APPLE__
#include <stdlib.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

int morph_random_bytes(unsigned char *buf, size_t len)
{
	if (len == 0)
		return 0;
	if (!buf)
		MORPH_RETURN(-EINVAL);
#ifdef __APPLE__
	arc4random_buf(buf, len);
	return 0;
#else
	int fd;
	size_t off;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		MORPH_RETURN_ERRNO();
	off = 0;
	while (off < len) {
		ssize_t rd = read(fd, buf + off, len - off);
		if (rd < 0) {
			int err = errno;

			close(fd);
			MORPH_RETURN(-err);
		}
		if (rd == 0) {
			close(fd);
			MORPH_RETURN(-EIO);
		}
		off += (size_t)rd;
	}
	close(fd);
	return 0;
#endif
}

static void hex_encode(const unsigned char *data, size_t len, char *out)
{
	static const char h[] = "0123456789abcdef";

	for (size_t i = 0; i < len; i++) {
		out[i * 2] = h[data[i] >> 4];
		out[i * 2 + 1] = h[data[i] & 0x0fU];
	}
	out[len * 2] = '\0';
}

int morph_random_id(const char *prefix, char *out, size_t out_size)
{
	unsigned char raw[16];
	char hex[sizeof(raw) * 2 + 1];
	int rc;

	if (!prefix || !out)
		MORPH_RETURN(-EINVAL);
	rc = morph_random_bytes(raw, sizeof(raw));
	if (rc < 0)
		return rc;
	hex_encode(raw, sizeof(raw), hex);
	if (snprintf(out, out_size, "%s%s", prefix, hex) >= (int)out_size)
		MORPH_RETURN(-ENOSPC);
	return 0;
}
