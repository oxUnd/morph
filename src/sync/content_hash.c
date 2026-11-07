#include "content_hash.h"
#include "util/error.h"
#include "blake3.h"

#include <errno.h>
#include <stdio.h>

static void hash_to_hex(const unsigned char hash[BLAKE3_OUT_LEN], char out[MORPH_SYNC_HASH_LEN])
{
	static const char hex[] = "0123456789abcdef";

	for (size_t i = 0; i < BLAKE3_OUT_LEN; i++) {
		out[i * 2] = hex[hash[i] >> 4];
		out[i * 2 + 1] = hex[hash[i] & 0x0f];
	}
	out[BLAKE3_OUT_LEN * 2] = '\0';
}

void sync_hash_bytes(const void *data, size_t len, char out[MORPH_SYNC_HASH_LEN])
{
	unsigned char hash[BLAKE3_OUT_LEN];
	blake3_hasher hasher;

	blake3_hasher_init(&hasher);
	blake3_hasher_update(&hasher, data, len);
	blake3_hasher_finalize(&hasher, hash, sizeof(hash));
	hash_to_hex(hash, out);
}

int sync_hash_file(const char *path, char out[MORPH_SYNC_HASH_LEN])
{
	unsigned char buffer[BUFSIZ];
	unsigned char hash[BLAKE3_OUT_LEN];
	blake3_hasher hasher;
	FILE *file;
	size_t read_size;

	file = fopen(path, "rb");
	if (!file)
		return morph_error_from_errno(errno);
	blake3_hasher_init(&hasher);
	while ((read_size = fread(buffer, 1, sizeof(buffer), file)) > 0)
		blake3_hasher_update(&hasher, buffer, read_size);
	if (ferror(file)) {
		fclose(file);
		return -EIO;
	}
	fclose(file);
	blake3_hasher_finalize(&hasher, hash, sizeof(hash));
	hash_to_hex(hash, out);
	return 0;
}
