#ifndef MORPH_MEMORY_REPOSITORY_H
#define MORPH_MEMORY_REPOSITORY_H

#include "memory_store.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct memory_episode_input {
	const char *task_type;
	const char *summary;
	const char *outcome;
	const char *entities;
	const char *decisions;
	const char *artifacts;
	const char *tools;
	int success;
	double importance;
	int64_t job_id;
};

int memory_store_write_fact(struct db *db, int64_t session_id, const char *key_name,
			    const char *value_text, const char *source_text, const char *category,
			    double importance);
int memory_store_upsert_fact(struct db *db, int64_t session_id, const char *key_name,
			     const char *value_text, const char *source_text, const char *category,
			     double importance);
int memory_store_prepare_session(struct db *db, int64_t session_id);
int memory_store_generation(struct db *db, int64_t session_id, int64_t *generation);
/* Begin succeeds with a short write transaction open; finish it exactly once.
 * Generation begin also validates the fence before any derived writes. */
int memory_store_begin_write(struct db *db);
int memory_store_finish_write(struct db *db, int result);
int memory_store_begin_generation(struct db *db, int64_t session_id, int64_t expected);
int memory_store_clear(struct db *db, int64_t session_id, enum memory_clear_scope scope);
int memory_store_migrate_preferences(struct db *db, int64_t session_id,
				     int (*verify)(const char *key, const char *value,
						   const char *source));
int memory_store_event_token(struct db *db, int64_t session_id, const char *event_token, char *out,
			     size_t capacity);
int memory_store_save_profile(struct db *db, int64_t session_id, const char *text);
int memory_store_insert_episode(struct db *db, int64_t session_id,
				const struct memory_episode_input *input);

#ifdef __cplusplus
}
#endif

#endif
