/* security.c — random ids, Basic auth decoding, PBKDF2-SHA256 */
#define _GNU_SOURCE
#include "security.h"
#include "util/error.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef __APPLE__
#include <fcntl.h>
#include <unistd.h>
#endif

#define SHA256_BLOCK_SIZE 64
#define SHA256_DIGEST_SIZE 32
#define PWD_SALT_BYTES 16
#define PWD_HASH_BYTES 32
#define PWD_ITERATIONS 120000

struct sha256_ctx {
	uint32_t state[8];
	uint64_t bitlen;
	unsigned char data[SHA256_BLOCK_SIZE];
	size_t datalen;
};

static const uint32_t k256[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
	0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
	0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
	0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
	0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
	0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
	0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
	0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
	0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
	0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
	0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
	0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
	0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
	0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
	0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static uint32_t rotr32(uint32_t x, uint32_t n)
{
	return (x >> n) | (x << (32U - n));
}

static void sha256_transform(struct sha256_ctx *ctx, const unsigned char data[64])
{
	uint32_t a, b, c, d, e, f, g, h;
	uint32_t m[64];

	for (int i = 0; i < 16; i++) {
		m[i] = ((uint32_t)data[i * 4] << 24) |
		       ((uint32_t)data[i * 4 + 1] << 16) |
		       ((uint32_t)data[i * 4 + 2] << 8) |
		       (uint32_t)data[i * 4 + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = rotr32(m[i - 15], 7) ^ rotr32(m[i - 15], 18) ^
			      (m[i - 15] >> 3);
		uint32_t s1 = rotr32(m[i - 2], 17) ^ rotr32(m[i - 2], 19) ^
			      (m[i - 2] >> 10);
		m[i] = m[i - 16] + s0 + m[i - 7] + s1;
	}

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];
	f = ctx->state[5];
	g = ctx->state[6];
	h = ctx->state[7];

	for (int i = 0; i < 64; i++) {
		uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t t1 = h + s1 + ch + k256[i] + m[i];
		uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = s0 + maj;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

static void sha256_init(struct sha256_ctx *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->state[0] = 0x6a09e667U;
	ctx->state[1] = 0xbb67ae85U;
	ctx->state[2] = 0x3c6ef372U;
	ctx->state[3] = 0xa54ff53aU;
	ctx->state[4] = 0x510e527fU;
	ctx->state[5] = 0x9b05688cU;
	ctx->state[6] = 0x1f83d9abU;
	ctx->state[7] = 0x5be0cd19U;
}

static void sha256_update(struct sha256_ctx *ctx, const unsigned char *data,
			  size_t len)
{
	for (size_t i = 0; i < len; i++) {
		ctx->data[ctx->datalen++] = data[i];
		if (ctx->datalen == SHA256_BLOCK_SIZE) {
			sha256_transform(ctx, ctx->data);
			ctx->bitlen += SHA256_BLOCK_SIZE * 8U;
			ctx->datalen = 0;
		}
	}
}

static void sha256_final(struct sha256_ctx *ctx, unsigned char hash[32])
{
	size_t i = ctx->datalen;

	ctx->data[i++] = 0x80U;
	if (i > 56) {
		while (i < 64)
			ctx->data[i++] = 0;
		sha256_transform(ctx, ctx->data);
		i = 0;
	}
	while (i < 56)
		ctx->data[i++] = 0;

	ctx->bitlen += ctx->datalen * 8U;
	for (int j = 0; j < 8; j++)
		ctx->data[63 - j] = (unsigned char)(ctx->bitlen >> (j * 8));
	sha256_transform(ctx, ctx->data);

	for (i = 0; i < 4; i++) {
		for (int j = 0; j < 8; j++) {
			size_t idx = (size_t)j * 4 + i;
			hash[idx] =
				(unsigned char)(ctx->state[j] >> (24 - i * 8));
		}
	}
}

static void hmac_sha256(const unsigned char *key, size_t key_len,
			const unsigned char *data, size_t data_len,
			unsigned char out[32])
{
	unsigned char k0[SHA256_BLOCK_SIZE];
	unsigned char ipad[SHA256_BLOCK_SIZE];
	unsigned char opad[SHA256_BLOCK_SIZE];
	unsigned char inner[SHA256_DIGEST_SIZE];
	struct sha256_ctx ctx;

	memset(k0, 0, sizeof(k0));
	if (key_len > SHA256_BLOCK_SIZE) {
		sha256_init(&ctx);
		sha256_update(&ctx, key, key_len);
		sha256_final(&ctx, k0);
	} else {
		memcpy(k0, key, key_len);
	}

	for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
		ipad[i] = (unsigned char)(k0[i] ^ 0x36U);
		opad[i] = (unsigned char)(k0[i] ^ 0x5cU);
	}

	sha256_init(&ctx);
	sha256_update(&ctx, ipad, sizeof(ipad));
	sha256_update(&ctx, data, data_len);
	sha256_final(&ctx, inner);

	sha256_init(&ctx);
	sha256_update(&ctx, opad, sizeof(opad));
	sha256_update(&ctx, inner, sizeof(inner));
	sha256_final(&ctx, out);
}

static void pbkdf2_sha256(const char *password, const unsigned char *salt,
			  size_t salt_len, int iterations,
			  unsigned char *out, size_t out_len)
{
	unsigned char u[SHA256_DIGEST_SIZE];
	unsigned char t[SHA256_DIGEST_SIZE];
	unsigned char block[128];
	uint32_t block_index = 1;
	size_t done = 0;
	size_t pass_len = strlen(password);

	while (done < out_len) {
		size_t chunk = out_len - done;
		if (chunk > SHA256_DIGEST_SIZE)
			chunk = SHA256_DIGEST_SIZE;

		memcpy(block, salt, salt_len);
		block[salt_len] = (unsigned char)(block_index >> 24);
		block[salt_len + 1] = (unsigned char)(block_index >> 16);
		block[salt_len + 2] = (unsigned char)(block_index >> 8);
		block[salt_len + 3] = (unsigned char)block_index;

		hmac_sha256((const unsigned char *)password, pass_len,
			    block, salt_len + 4, u);
		memcpy(t, u, sizeof(t));

		for (int i = 1; i < iterations; i++) {
			hmac_sha256((const unsigned char *)password, pass_len,
				    u, sizeof(u), u);
			for (int j = 0; j < SHA256_DIGEST_SIZE; j++)
				t[j] ^= u[j];
		}

		memcpy(out + done, t, chunk);
		done += chunk;
		block_index++;
	}
}

static int random_bytes(unsigned char *buf, size_t len)
{
#ifdef __APPLE__
	arc4random_buf(buf, len);
	return 0;
#else
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		MORPH_RETURN(-errno);
	size_t off = 0;
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

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int hex_decode(const char *hex, unsigned char *out, size_t out_len)
{
	if (strlen(hex) != out_len * 2)
		return -EINVAL;
	for (size_t i = 0; i < out_len; i++) {
		int hi = hex_value(hex[i * 2]);
		int lo = hex_value(hex[i * 2 + 1]);
		if (hi < 0 || lo < 0)
			return -EINVAL;
		out[i] = (unsigned char)((hi << 4) | lo);
	}
	return 0;
}

int fcgi_random_id(const char *prefix, char *out, size_t out_size)
{
	unsigned char raw[16];
	char hex[sizeof(raw) * 2 + 1];
	int rc;

	if (!prefix || !out)
		MORPH_RETURN(-EINVAL);
	rc = random_bytes(raw, sizeof(raw));
	if (rc < 0)
		return rc;
	hex_encode(raw, sizeof(raw), hex);
	if (snprintf(out, out_size, "%s%s", prefix, hex) >= (int)out_size)
		MORPH_RETURN(-ENOSPC);
	return 0;
}

int fcgi_password_hash(const char *password, char *out, size_t out_size)
{
	unsigned char salt[PWD_SALT_BYTES];
	unsigned char hash[PWD_HASH_BYTES];
	char salt_hex[PWD_SALT_BYTES * 2 + 1];
	char hash_hex[PWD_HASH_BYTES * 2 + 1];
	int rc;

	if (!password || !out || !*password)
		MORPH_RETURN(-EINVAL);
	rc = random_bytes(salt, sizeof(salt));
	if (rc < 0)
		return rc;
	pbkdf2_sha256(password, salt, sizeof(salt), PWD_ITERATIONS,
		      hash, sizeof(hash));
	hex_encode(salt, sizeof(salt), salt_hex);
	hex_encode(hash, sizeof(hash), hash_hex);
	if (snprintf(out, out_size, "pbkdf2-sha256$%d$%s$%s",
		     PWD_ITERATIONS, salt_hex, hash_hex) >= (int)out_size)
		MORPH_RETURN(-ENOSPC);
	return 0;
}

static int ct_eq(const unsigned char *a, const unsigned char *b, size_t len)
{
	unsigned char diff = 0;
	for (size_t i = 0; i < len; i++)
		diff |= (unsigned char)(a[i] ^ b[i]);
	return diff == 0;
}

int fcgi_password_verify(const char *password, const char *encoded)
{
	char tmp[256];
	char *algo;
	char *iter_s;
	char *salt_s;
	char *hash_s;
	char *save = NULL;
	int iterations;
	unsigned char salt[PWD_SALT_BYTES];
	unsigned char expected[PWD_HASH_BYTES];
	unsigned char actual[PWD_HASH_BYTES];

	if (!password || !encoded)
		return 0;
	if (strlen(encoded) >= sizeof(tmp))
		return 0;
	snprintf(tmp, sizeof(tmp), "%s", encoded);

	algo = strtok_r(tmp, "$", &save);
	iter_s = strtok_r(NULL, "$", &save);
	salt_s = strtok_r(NULL, "$", &save);
	hash_s = strtok_r(NULL, "$", &save);
	if (!algo || !iter_s || !salt_s || !hash_s)
		return 0;
	if (strcmp(algo, "pbkdf2-sha256") != 0)
		return 0;
	iterations = atoi(iter_s);
	if (iterations < 10000)
		return 0;
	if (hex_decode(salt_s, salt, sizeof(salt)) < 0)
		return 0;
	if (hex_decode(hash_s, expected, sizeof(expected)) < 0)
		return 0;

	pbkdf2_sha256(password, salt, sizeof(salt), iterations,
		      actual, sizeof(actual));
	return ct_eq(actual, expected, sizeof(actual));
}

static int b64_value(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	if (c == '=')
		return -2;
	return -1;
}

static int b64_decode(const char *in, unsigned char *out, size_t out_cap,
		      size_t *out_len)
{
	size_t len = strlen(in);
	size_t pos = 0;

	if (len % 4 != 0)
		return -EINVAL;
	for (size_t i = 0; i < len; i += 4) {
		int v[4];
		for (int j = 0; j < 4; j++) {
			v[j] = b64_value(in[i + (size_t)j]);
			if (v[j] == -1)
				return -EINVAL;
		}
		if (pos >= out_cap)
			return -ENOSPC;
		out[pos++] = (unsigned char)((v[0] << 2) | (v[1] >> 4));
		if (v[2] != -2) {
			if (pos >= out_cap)
				return -ENOSPC;
			out[pos++] =
				(unsigned char)(((v[1] & 15) << 4) | (v[2] >> 2));
		}
		if (v[3] != -2) {
			if (pos >= out_cap)
				return -ENOSPC;
			out[pos++] =
				(unsigned char)(((v[2] & 3) << 6) | v[3]);
		}
	}
	*out_len = pos;
	return 0;
}

int fcgi_basic_decode(const char *auth_hdr, char *user, size_t user_size,
		      char *pass, size_t pass_size)
{
	unsigned char decoded[512];
	size_t decoded_len = 0;
	char *colon;
	int rc;

	if (!auth_hdr || strncmp(auth_hdr, "Basic ", 6) != 0)
		return 0;
	rc = b64_decode(auth_hdr + 6, decoded, sizeof(decoded) - 1, &decoded_len);
	if (rc < 0)
		return 0;
	decoded[decoded_len] = '\0';
	colon = strchr((char *)decoded, ':');
	if (!colon)
		return 0;
	*colon = '\0';
	if (snprintf(user, user_size, "%s", decoded) >= (int)user_size)
		return 0;
	if (snprintf(pass, pass_size, "%s", colon + 1) >= (int)pass_size)
		return 0;
	return user[0] && pass[0];
}
