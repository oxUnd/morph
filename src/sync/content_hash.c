#include "content_hash.h"
#include "errors.h"
#include "blake3.h"
#include "nb/encoding.h"

#include <errno.h>
#include <stdio.h>

void sync_hash_bytes(const void *data, size_t len, char out[MORPH_SYNC_HASH_LEN])
{
	unsigned char hash[BLAKE3_OUT_LEN];
	blake3_hasher hasher;

	blake3_hasher_init(&hasher);
	blake3_hasher_update(&hasher, data, len);
	blake3_hasher_finalize(&hasher, hash, sizeof(hash));
	(void)nb_hex_encode_to(hash, sizeof(hash), false, out, MORPH_SYNC_HASH_LEN, NULL);
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
		return nb_status_from_errno(errno, NULL);
	blake3_hasher_init(&hasher);
	while ((read_size = fread(buffer, 1, sizeof(buffer), file)) > 0)
		blake3_hasher_update(&hasher, buffer, read_size);
	if (ferror(file)) {
		fclose(file);
		return NB_ERR_IO;
	}
	fclose(file);
	blake3_hasher_finalize(&hasher, hash, sizeof(hash));
	(void)nb_hex_encode_to(hash, sizeof(hash), false, out, MORPH_SYNC_HASH_LEN, NULL);
	return 0;
}
