#ifndef MORPH_MEMORY_INTERNAL_H
#define MORPH_MEMORY_INTERNAL_H

#include "db/database.h"
#include "nb/buf.h"
#include "nb/cleanup.h"

int memory_prepare_session(struct db *db, int64_t session_id);
int memory_read_version(struct db *db, int64_t session_id, int64_t *memory_version);

int memory_appendf(nb_buf *b, size_t max_len, const char *fmt, ...);

struct memory_options;
int memory_refresh_summaries_async(struct db *db, int64_t session_id,
				   const struct memory_options *options);
int memory_organize_summary(struct db *db, int64_t session_id,
			    const struct memory_options *options);

#endif
