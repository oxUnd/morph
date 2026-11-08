#ifndef MORPH_MEMORY_SUMMARY_STORE_H
#define MORPH_MEMORY_SUMMARY_STORE_H

#include "db/database.h"

#ifdef __cplusplus
extern "C" {
#endif

int memory_summary_revision(struct db *db, int64_t session_id, int64_t *revision);
/* Caller owns the write transaction and has checked its captured memory version. */
int memory_summary_save(struct db *db, int64_t session_id, int64_t version, int64_t revision,
			const char *title, const char *text, const char *sources);
int memory_note_save(struct db *db, int64_t session_id, const char *title, const char *text,
		     const char *source, const char *event_token, const char *scope);
/* An exact value and version prevent overwriting a concurrent edit. Changes
 * advance the source version, preventing older learning jobs from restoring it. */
int memory_record_edit(struct db *db, int64_t session_id, const char *kind, int64_t id,
		       int64_t version, const char *expected_value, const char *replacement,
		       const char *source);

#ifdef __cplusplus
}
#endif
#endif
