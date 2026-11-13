#include "memory_job_store.h"

#include "nb/cleanup.h"
#include "errors.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void memory_job_records_cleanup(struct memory_job_records *records)
{
	if (!records)
		return;
	for (size_t i = 0; i < records->rows.len; i++) {
		struct memory_job_record *row = nb_vec_at(&records->rows, i);
		free(row->payload_json);
	}
	nb_vec_cleanup(&records->rows);
	memset(records, 0, sizeof(*records));
}

static void memory_job_records_move(struct memory_job_records *destination,
				    struct memory_job_records *source)
{
	*destination = *source;
	memset(source, 0, sizeof(*source));
}

static int read_records(struct db *db, const char *sql, int64_t session_id, int limit,
			struct memory_job_records *out)
{
	struct memory_job_records value NB_CLEANUP(memory_job_records_cleanup) = {0};
	struct memory_job_record row;
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!db || !db->handle || !out)
		return NB_ERR_INVAL;
	if (((void *)out->rows.data))
		return NB_ERR_EXISTS;
	rc = (nb_vec_init(&value.rows, sizeof(row)),
	      nb_vec_reserve(&value.rows, 16) ? 0 : NB_ERR_NOMEM);
	if (rc) {
		return rc;
	}
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
		return MORPH_ERR_DB;
	}
	if (limit > 0) {
		sqlite3_bind_int64(stmt, 1, session_id);
		sqlite3_bind_int(stmt, 2, limit);
	}
	rc = 0;
	int step;

	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *payload = (const char *)sqlite3_column_text(stmt, 3);
		const char *state = (const char *)sqlite3_column_text(stmt, 4);

		memset(&row, 0, sizeof(row));
		row.id = sqlite3_column_int64(stmt, 0);
		row.session_id = sqlite3_column_int64(stmt, 1);
		row.memory_version = sqlite3_column_int64(stmt, 2);
		row.worker_pid = sqlite3_column_int64(stmt, 5);
		row.attempts = sqlite3_column_int(stmt, 6);
		row.error_code = sqlite3_column_int(stmt, 7);
		snprintf(row.stage, sizeof(row.stage), "%s",
			 (const char *)sqlite3_column_text(stmt, 8));
		snprintf(row.state, sizeof(row.state), "%s", state ? state : "");
		row.payload_json = payload ? strdup(payload) : NULL;
		if (payload && !row.payload_json) {
			rc = NB_ERR_NOMEM;
			break;
		}
		struct memory_job_record *slot = nb_vec_push(&value.rows);

		if (!slot) {
			free(row.payload_json);
			rc = NB_ERR_NOMEM;
			break;
		}
		*slot = row;
		row.payload_json = NULL;
	}
	sqlite3_finalize(stmt);
	if (!rc && step != SQLITE_DONE)
		rc = MORPH_ERR_DB;
	if (rc == 0)
		memory_job_records_move(out, &value);
	return rc;
}

int memory_job_store_recoverable(struct db *db, struct memory_job_records *out)
{
	return read_records(
		db,
		"SELECT "
		"id,session_id,generation,payload,state,worker_pid,attempts,error_code,stage "
		"FROM memory_jobs WHERE state IN ('queued','running') OR "
		"(state='failed' AND attempts<3 AND updated_at<CAST(strftime('%s','now') "
		"AS INTEGER)-30) ORDER BY id",
		0, 0, out);
}

int memory_job_store_recent(struct db *db, int64_t session_id, int limit,
			    struct memory_job_records *out)
{
	if (session_id < 0 || limit <= 0)
		return NB_ERR_INVAL;
	return read_records(
		db,
		"SELECT id,session_id,generation,NULL,state,worker_pid,attempts,error_code,stage "
		"FROM memory_jobs WHERE session_id=? ORDER BY id DESC LIMIT ?",
		session_id, limit, out);
}

int memory_job_store_add(struct db *db, int64_t session_id, int64_t memory_version,
			 const char *payload_json, int64_t *id)
{
	sqlite3_stmt *stmt = NULL;
	sqlite3_mutex *mutex;
	int rc;

	if (!db || !db->handle || session_id <= 0 || !payload_json || !id)
		return NB_ERR_INVAL;
	*id = 0;
	mutex = sqlite3_db_mutex(db->handle);
	if (!mutex)
		return NB_ERR_UNSUPPORTED;
	/* Keep the insert and its connection-local identity indivisible. */
	sqlite3_mutex_enter(mutex);
	rc = sqlite3_prepare_v2(
		db->handle,
		"INSERT INTO memory_jobs(session_id,generation,payload,content_revision) "
		"VALUES(?1,?2,?3,COALESCE((SELECT content_revision FROM memory_scopes "
		"WHERE session_id=?1),0))",
		-1, &stmt, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, session_id);
		sqlite3_bind_int64(stmt, 2, memory_version);
		sqlite3_bind_text(stmt, 3, payload_json, -1, SQLITE_TRANSIENT);
		rc = sqlite3_step(stmt);
	}
	if (rc == SQLITE_DONE)
		*id = sqlite3_last_insert_rowid(db->handle);
	sqlite3_finalize(stmt);
	sqlite3_mutex_leave(mutex);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int change_job(struct db *db, int64_t id, const char *sql, const char *state, int64_t value)
{
	sqlite3_stmt *stmt = NULL;
	sqlite3_mutex *mutex;
	int rc;

	if (!db || !db->handle || id <= 0)
		return NB_ERR_INVAL;
	mutex = sqlite3_db_mutex(db->handle);
	if (!mutex)
		return NB_ERR_UNSUPPORTED;
	sqlite3_mutex_enter(mutex);
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc == SQLITE_OK) {
		if (state)
			sqlite3_bind_text(stmt, 1, state, -1, SQLITE_TRANSIENT);
		else
			sqlite3_bind_int64(stmt, 1, value);
		sqlite3_bind_int64(stmt, 2, id);
		if (sqlite3_bind_parameter_count(stmt) == 3)
			sqlite3_bind_int64(stmt, 3, value);
		rc = sqlite3_step(stmt);
	}
	int changed = rc == SQLITE_DONE ? sqlite3_changes(db->handle) : MORPH_ERR_DB;

	sqlite3_finalize(stmt);
	sqlite3_mutex_leave(mutex);
	return changed;
}

int memory_job_store_claim(struct db *db, int64_t id, int64_t worker_pid)
{
	return change_job(
		db, id,
		"UPDATE memory_jobs SET state='running',attempts=attempts+1,worker_pid=?1,"
		"updated_at=CAST(strftime('%s','now') AS INTEGER) WHERE id=?2 "
		"AND state='queued'",
		NULL, worker_pid);
}

int memory_job_store_requeue(struct db *db, int64_t id, const char *expected_state)
{
	if (!expected_state)
		return NB_ERR_INVAL;
	return change_job(db, id, "UPDATE memory_jobs SET state='queued' WHERE state=?1 AND id=?2",
			  expected_state, 0);
}

int memory_job_store_complete(struct db *db, int64_t id, int error)
{
	const char *state = error == NB_ERR_STALE ? "cancelled" : error ? "failed" : "completed";
	int rc = change_job(
		db, id,
		"UPDATE memory_jobs SET state=?1,error_code=?3,content_revision=COALESCE("
		"(SELECT content_revision FROM memory_scopes WHERE "
		"session_id=memory_jobs.session_id),0),"
		"updated_at=CAST(strftime('%s','now') AS INTEGER) WHERE id=?2 AND state='running'",
		state, error);

	return rc < 0 ? rc : 0;
}

int memory_job_store_stage(struct db *db, int64_t id, const char *stage)
{
	if (!stage || (strcmp(stage, "extract") && strcmp(stage, "organize")))
		return NB_ERR_INVAL;
	int rc = change_job(db, id,
			    "UPDATE memory_jobs SET stage=?1,content_revision=COALESCE((SELECT "
			    "content_revision "
			    "FROM memory_scopes WHERE session_id=memory_jobs.session_id),0),"
			    "updated_at=CAST(strftime('%s','now') AS INTEGER) "
			    "WHERE id=?2 AND state IN ('queued','running')",
			    stage, 0);

	return rc < 0 ? rc : 0;
}

int memory_job_store_retry_session(struct db *db, int64_t session_id)
{
	return change_job(
		db, session_id,
		"UPDATE memory_jobs SET state='queued',attempts=0,error_code=0,worker_pid=?1 "
		"WHERE session_id=?2 AND state='failed'",
		NULL, 0);
}
