#ifndef MORPH_MEMORY_JOB_STORE_H
#define MORPH_MEMORY_JOB_STORE_H

#include "db/database.h"
#include "nb/err.h"
#include "nb/vec.h"
#include "nb/cleanup.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEMORY_JOB_STATE_MAX 16

struct memory_job_record {
	int64_t id;
	int64_t session_id;
	int64_t memory_version;
	int64_t worker_pid;
	char *payload_json;
	char state[MEMORY_JOB_STATE_MAX];
	char stage[MEMORY_JOB_STATE_MAX];
	int attempts;
	int error_code;
};

struct memory_job_records {
	nb_vec rows;
};

/* Outputs must be zero-initialized and empty. Occupied owners return NB_ERR_EXISTS. */
/* Record strings are copied and remain valid until records cleanup. */
int memory_job_store_recoverable(struct db *db, struct memory_job_records *out);
int memory_job_store_recent(struct db *db, int64_t session_id, int limit,
			    struct memory_job_records *out);
void memory_job_records_cleanup(struct memory_job_records *records);
int memory_job_store_add(struct db *db, int64_t session_id, int64_t memory_version,
			 const char *payload_json, int64_t *id);
/* Claim/requeue return one when the expected state matched, otherwise zero. */
int memory_job_store_claim(struct db *db, int64_t id, int64_t worker_pid);
int memory_job_store_requeue(struct db *db, int64_t id, const char *expected_state);
int memory_job_store_stage(struct db *db, int64_t id, const char *stage);
int memory_job_store_retry_session(struct db *db, int64_t session_id);
int memory_job_store_complete(struct db *db, int64_t id, int error);

#ifdef __cplusplus
}
#endif

#endif
