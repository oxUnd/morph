#ifndef MORPH_MEMORY_INTERNAL_H
#define MORPH_MEMORY_INTERNAL_H

#include "db/database.h"
#include "util/buf.h"

int memory_prepare_session(struct db *db, int64_t session_id);
int memory_generation(struct db *db, int64_t session_id, int64_t *generation);

int memory_appendf(morph_buf_t *b, size_t max_len, const char *fmt, ...);

int memory_contains_ci(const char *haystack, const char *needle);

#endif
