#ifndef MORPH_MEMORY_JOB_STORE_H
#define MORPH_MEMORY_JOB_STORE_H

#include "db/database.h"
#include "util/array.h"

#ifdef __cplusplus
extern "C" {
#endif

struct arena;

#define MEMORY_JOB_STATE_MAX 16

struct memory_job_record {
	int64_t id;
	int64_t session_id;
	int64_t generation;
	int64_t worker_pid;
	const char *payload_json;
	char state[MEMORY_JOB_STATE_MAX];
	int attempts;
	int error_code;
};

struct memory_job_records {
	struct arena *arena;
	morph_array_t rows;
};

/* Record strings are copied and remain valid until records cleanup. */
int memory_job_store_recoverable(struct db *db, struct memory_job_records *out);
int memory_job_store_recent(struct db *db, int64_t session_id, int limit,
			    struct memory_job_records *out);
void memory_job_records_cleanup(struct memory_job_records *records);
int memory_job_store_add(struct db *db, int64_t session_id, int64_t generation,
			 const char *payload_json, int64_t *id);
/* Claim/requeue return one when the expected state matched, otherwise zero. */
int memory_job_store_claim(struct db *db, int64_t id, int64_t worker_pid);
int memory_job_store_requeue(struct db *db, int64_t id, const char *expected_state);
int memory_job_store_complete(struct db *db, int64_t id, int error);

#ifdef __cplusplus
}
#endif

#endif
