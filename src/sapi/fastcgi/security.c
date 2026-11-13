/* security.c — random ids, Basic auth decoding, PBKDF2-SHA256 */
#define _GNU_SOURCE
#include "security.h"
#include "errors.h"
#include "nb/random.h"
#include "nb/encoding.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nb/hash.h"
#include "nb/encoding.h"
#include "nb/cleanup.h"
#define PWD_SALT_BYTES 16
#define PWD_HASH_BYTES 32
#define PWD_ITERATIONS 120000

static void pbkdf2_sha256(const char *password, const unsigned char *salt,
			  size_t salt_len, int iterations,
			  unsigned char *out, size_t out_len)
{
	unsigned char u[NB_SHA256_DIGEST_SIZE];
	unsigned char t[NB_SHA256_DIGEST_SIZE];
	unsigned char block[128];
	uint32_t block_index = 1;
	size_t done = 0;
	size_t pass_len = strlen(password);

	while (done < out_len) {
		size_t chunk = out_len - done;
		if (chunk > NB_SHA256_DIGEST_SIZE)
			chunk = NB_SHA256_DIGEST_SIZE;

		memcpy(block, salt, salt_len);
		block[salt_len] = (unsigned char)(block_index >> 24);
		block[salt_len + 1] = (unsigned char)(block_index >> 16);
		block[salt_len + 2] = (unsigned char)(block_index >> 8);
		block[salt_len + 3] = (unsigned char)block_index;

		nb_hmac_sha256((const unsigned char *)password, pass_len, block, salt_len + 4, u);
		memcpy(t, u, sizeof(t));

		for (int i = 1; i < iterations; i++) {
			nb_hmac_sha256((const unsigned char *)password, pass_len, u, sizeof(u), u);
			for (size_t j = 0; j < NB_SHA256_DIGEST_SIZE; j++)
				t[j] ^= u[j];
		}

		memcpy(out + done, t, chunk);
		done += chunk;
		block_index++;
	}
}

int fcgi_random_id(const char *prefix, char *out, size_t out_size)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;

	return ((file_status_1 = (!(prefix) || (16) == 0 || (16) > 16 ? NB_ERR_INVAL
				  : !(nb_buf_clear(&file_value_1),
				      nb_buf_append_cstr(&file_value_1, prefix))
					  ? NB_ERR_NOMEM
					  : nb_random_hex(&file_value_1, 16, NULL))) != NB_OK
			? file_status_1
		: (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), out, out_size)) ==
				NB_ERR_RANGE
			? NB_ERR_NO_SPACE
			: file_status_1);
}

int fcgi_password_hash(const char *password, char *out, size_t out_size)
{
	unsigned char salt[PWD_SALT_BYTES];
	unsigned char hash[PWD_HASH_BYTES];
	char salt_hex[PWD_SALT_BYTES * 2 + 1];
	char hash_hex[PWD_HASH_BYTES * 2 + 1];
	int rc;

	if (!password || !out || !*password)
		return NB_ERR_INVAL;
	rc = nb_random_bytes(salt, sizeof(salt), NULL);
	if (rc < 0)
		return rc;
	pbkdf2_sha256(password, salt, sizeof(salt), PWD_ITERATIONS,
		      hash, sizeof(hash));
	(void)nb_hex_encode_to(salt, sizeof(salt), false, salt_hex, sizeof(salt_hex), NULL);
	(void)nb_hex_encode_to(hash, sizeof(hash), false, hash_hex, sizeof(hash_hex), NULL);
	if (snprintf(out, out_size, "pbkdf2-sha256$%d$%s$%s",
		     PWD_ITERATIONS, salt_hex, hash_hex) >= (int)out_size)
		return NB_ERR_NO_SPACE;
	return 0;
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
	nb_buf salt NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf expected NB_CLEANUP(nb_buf_destroy) = {0};
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
	if (strlen(salt_s) != PWD_SALT_BYTES * 2 ||
	    nb_hex_decode(nb_str_from_cstr(salt_s), &salt, NULL) != NB_OK)
		return 0;
	if (strlen(hash_s) != PWD_HASH_BYTES * 2 ||
	    nb_hex_decode(nb_str_from_cstr(hash_s), &expected, NULL) != NB_OK)
		return 0;

	pbkdf2_sha256(password, (const unsigned char *)nb_buf_cstr(&salt), nb_buf_len(&salt),
		      iterations, actual, sizeof(actual));
	return nb_hash_equal(actual, nb_buf_cstr(&expected), sizeof(actual));
}

int fcgi_basic_decode(const char *auth_hdr, char *user, size_t user_size,
		      char *pass, size_t pass_size)
{
	nb_buf decoded NB_CLEANUP(nb_buf_destroy) = {0};
	char *colon;

	if (!auth_hdr || strncmp(auth_hdr, "Basic ", 6) != 0)
		return 0;
	const char *payload = auth_hdr + 6;
	size_t payload_len = strlen(payload);
	if (!payload_len || payload_len % 4 || payload_len > 684 || strpbrk(payload, " \t\r\n") ||
	    nb_base64_decode(nb_str_from_parts(payload, payload_len), NB_B64_STD, &decoded, NULL) !=
		    NB_OK ||
	    nb_buf_len(&decoded) > 511 || memchr(nb_buf_cstr(&decoded), 0, nb_buf_len(&decoded)))
		return 0;
	colon = strchr(decoded.ptr, ':');
	if (!colon)
		return 0;
	*colon = '\0';
	if (snprintf(user, user_size, "%s", nb_buf_cstr(&decoded)) >= (int)user_size)
		return 0;
	if (snprintf(pass, pass_size, "%s", colon + 1) >= (int)pass_size)
		return 0;
	return user[0] && pass[0];
}
