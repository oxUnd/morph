#ifndef MORPH_SYNC_CONTENT_HASH_H
#define MORPH_SYNC_CONTENT_HASH_H

#include "sync.h"
#include <stddef.h>

void sync_hash_bytes(const void *data, size_t size, char out[MORPH_SYNC_HASH_LEN]);
int sync_hash_file(const char *path, char out[MORPH_SYNC_HASH_LEN]);

#endif
