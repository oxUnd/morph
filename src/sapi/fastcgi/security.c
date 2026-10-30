/* security.c — random ids, Basic auth decoding, PBKDF2-SHA256 */
#define _GNU_SOURCE
#include "security.h"
#include "util/error.h"
#include "util/id.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/sha256.h"
#define PWD_SALT_BYTES 16
#define PWD_HASH_BYTES 32
#define PWD_ITERATIONS 120000

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
	return morph_random_id(prefix, out, out_size);
}

int fcgi_password_hash(const char *password, char *out, size_t out_size)
{
	unsigned char salt[PWD_SALT_BYTES];
	unsigned char hash[PWD_HASH_BYTES];
	char salt_hex[PWD_SALT_BYTES * 2 + 1];
	char hash_hex[PWD_HASH_BYTES * 2 + 1];
	int rc;

	if (!password || !out || !*password)
		return -EINVAL;
	rc = morph_random_bytes(salt, sizeof(salt));
	if (rc < 0)
		return rc;
	pbkdf2_sha256(password, salt, sizeof(salt), PWD_ITERATIONS,
		      hash, sizeof(hash));
	hex_encode(salt, sizeof(salt), salt_hex);
	hex_encode(hash, sizeof(hash), hash_hex);
	if (snprintf(out, out_size, "pbkdf2-sha256$%d$%s$%s",
		     PWD_ITERATIONS, salt_hex, hash_hex) >= (int)out_size)
		return -ENOSPC;
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
