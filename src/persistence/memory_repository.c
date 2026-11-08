#include "memory_repository.h"

#include "preferences.h"
#include "util/error.h"
#include "util/cleanup.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int memory_store_begin_write(struct db *db)
{
	if (!db || !db->handle)
		return -EINVAL;
	return db_exec(db, "BEGIN IMMEDIATE");
}

int memory_store_finish_write(struct db *db, int result)
{
	if (result == 0)
		result = db_exec(db, "COMMIT");
	if (result != 0)
		(void)db_exec(db, "ROLLBACK");
	return result;
}

int memory_store_write_fact(struct db *db, int64_t session_id, const char *key_name,
			    const char *value_text, const char *source_text, const char *category,
			    double importance)
{
	sqlite3_stmt *stmt = NULL;
	const char *select_sql = "SELECT id, value_text FROM memory_facts "
				 "WHERE session_id=? AND key_name=? AND is_current=1 "
				 "ORDER BY updated_at DESC LIMIT 1";
	int rc;
	int64_t old_id = 0;
	char *old_value MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	int64_t now = (int64_t)time(NULL);

	if (!db || !db->handle || !key_name || !value_text || !*value_text)
		return -EINVAL;
	if (!category || !*category)
		category = "general";
	if (importance < 0.0 || importance > 1.0)
		importance = 0.5;

	rc = sqlite3_prepare_v2(db->handle, select_sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, key_name, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		old_id = sqlite3_column_int64(stmt, 0);
		{
			const char *text = (const char *)sqlite3_column_text(stmt, 1);
			if (text)
				old_value = strdup(text);
		}
	}
	sqlite3_finalize(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	if (old_id && !old_value)
		return -ENOMEM;

	/* Same value as the current fact: refresh metadata only, no
	 * supersession needed. */
	if (old_id > 0 && old_value && strcmp(old_value, value_text) == 0) {
		const char *update_sql =
			"UPDATE memory_facts "
			"SET source_text=?, updated_at=?, valid_to=NULL, is_current=1, "
			"category=?, importance=? "
			"WHERE id=?";
		rc = sqlite3_prepare_v2(db->handle, update_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK) {
			return -EIO;
		}
		sqlite3_bind_text(stmt, 1, source_text ? source_text : "", -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, now);
		sqlite3_bind_text(stmt, 3, category, -1, SQLITE_TRANSIENT);
		sqlite3_bind_double(stmt, 4, importance);
		sqlite3_bind_int64(stmt, 5, old_id);
		rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		return rc == SQLITE_DONE ? 0 : -EIO;
	}

	if (old_id > 0) {
		const char *expire_sql = "UPDATE memory_facts "
					 "SET is_current=0, valid_to=?, updated_at=? "
					 "WHERE id=?";
		rc = sqlite3_prepare_v2(db->handle, expire_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK)
			return -EIO;
		sqlite3_bind_int64(stmt, 1, now);
		sqlite3_bind_int64(stmt, 2, now);
		sqlite3_bind_int64(stmt, 3, old_id);
		rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE)
			return -EIO;
	}

	{
		const char *insert_sql =
			"INSERT INTO memory_facts("
			"session_id,key_name,value_text,source_text,confidence,"
			"category,importance,"
			"is_current,valid_from,valid_to,superseded_by,created_at,updated_at"
			") VALUES(?,?,?,?,1.0,?,?,1,?,NULL,NULL,?,?)";
		int64_t new_id;
		rc = sqlite3_prepare_v2(db->handle, insert_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK)
			return -EIO;
		sqlite3_bind_int64(stmt, 1, session_id);
		sqlite3_bind_text(stmt, 2, key_name, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, value_text, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, source_text ? source_text : "", -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, category, -1, SQLITE_TRANSIENT);
		sqlite3_bind_double(stmt, 6, importance);
		sqlite3_bind_int64(stmt, 7, now);
		sqlite3_bind_int64(stmt, 8, now);
		sqlite3_bind_int64(stmt, 9, now);
		rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE)
			return -EIO;
		new_id = sqlite3_last_insert_rowid(db->handle);
		if (old_id > 0) {
			const char *link_sql = "UPDATE memory_facts SET superseded_by=? WHERE id=?";
			rc = sqlite3_prepare_v2(db->handle, link_sql, -1, &stmt, NULL);
			if (rc != SQLITE_OK)
				return -EIO;
			sqlite3_bind_int64(stmt, 1, new_id);
			sqlite3_bind_int64(stmt, 2, old_id);
			rc = sqlite3_step(stmt);
			sqlite3_finalize(stmt);
			if (rc != SQLITE_DONE)
				return -EIO;
		}
	}

	return 0;
}

int memory_store_upsert_fact(struct db *db, int64_t session_id, const char *key_name,
			     const char *value_text, const char *source_text, const char *category,
			     double importance)
{
	struct db_lock lock MORPH_CLEANUP(db_lock_cleanup) = {0};
	int rc = db_lock_acquire(db, &lock);

	if (rc != 0)
		return rc;
	rc = memory_store_begin_write(db);
	if (rc != 0)
		return rc;
	rc = memory_store_write_fact(db, session_id, key_name, value_text, source_text, category,
				     importance);
	return memory_store_finish_write(db, rc);
}

int memory_store_prepare_session(struct db *db, int64_t session_id)
{
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db->handle, "SELECT 1 FROM memory_scopes WHERE session_id=?",
				    -1, &stmt, NULL);

	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc == SQLITE_ROW)
		return 0;
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	return preference_bind(db, session_id, "local", "");
}

int memory_store_read_version(struct db *db, int64_t session_id, int64_t *memory_version)
{
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db->handle,
				    "SELECT generation FROM memory_scopes WHERE session_id=?", -1,
				    &stmt, NULL);

	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW)
		*memory_version = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_ROW)
		return rc == SQLITE_DONE ? -ENOENT : MORPH_ERR_DB;
	return 0;
}

int memory_store_begin_versioned_write(struct db *db, int64_t session_id, int64_t expected)
{
	int64_t current = 0;
	int rc = memory_store_begin_write(db);

	if (rc != 0)
		return rc;
	rc = memory_store_read_version(db, session_id, &current);
	if (rc == 0 && current != expected)
		rc = -ESTALE;
	if (rc != 0)
		return memory_store_finish_write(db, rc);
	return 0;
}

int memory_store_clear(struct db *db, int64_t session_id, enum memory_clear_scope scope)
{
	sqlite3_stmt *stmt = NULL;
	struct db_lock lock MORPH_CLEANUP(db_lock_cleanup) = {0};
	int rc;

	if (!db || !db->handle || scope < MEMORY_CLEAR_ALL || scope > MEMORY_CLEAR_TIPS)
		return -EINVAL;
	rc = db_lock_acquire(db, &lock);
	if (rc != 0)
		return rc;
	rc = memory_store_prepare_session(db, session_id);
	if (rc != 0)
		return rc;
	rc = memory_store_begin_write(db);
	if (rc != 0)
		return rc;
	rc = sqlite3_prepare_v2(db->handle,
				"UPDATE memory_scopes SET generation=generation+1,migrated=1 "
				"WHERE session_id=?",
				-1, &stmt, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, session_id);
		rc = sqlite3_step(stmt);
	}
	sqlite3_finalize(stmt);
	rc = rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
	if (rc == 0) {
		switch (scope) {
		case MEMORY_CLEAR_ALL:
			rc = preference_clear_session(db, session_id);
			if (rc == 0)
				rc = memory_store_clear_all(db, session_id);
			break;
		case MEMORY_CLEAR_FACTS:
			rc = memory_store_clear_facts(db, session_id);
			break;
		case MEMORY_CLEAR_TASKS:
			rc = memory_store_clear_tasks(db, session_id);
			break;
		case MEMORY_CLEAR_TIPS:
			rc = memory_store_clear_tips(db, session_id);
			break;
		}
	}
	if (rc == 0 && scope == MEMORY_CLEAR_ALL) {
		rc = sqlite3_prepare_v2(db->handle,
					"DELETE FROM memory_candidates WHERE session_id=?", -1,
					&stmt, NULL);
		if (rc == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			rc = sqlite3_step(stmt);
		}
		sqlite3_finalize(stmt);
		rc = rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
	}
	if (rc == 0) {
		rc = sqlite3_prepare_v2(db->handle,
					"DELETE FROM memory_summaries WHERE session_id=?", -1,
					&stmt, NULL);
		if (rc == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			rc = sqlite3_step(stmt);
		}
		sqlite3_finalize(stmt);
		rc = rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
	}
	if (rc == 0) {
		rc = sqlite3_prepare_v2(
			db->handle,
			"UPDATE memory_jobs SET state=CASE WHEN state='completed' THEN state "
			"ELSE 'cancelled' END,payload=CASE WHEN ?=0 THEN '{}' ELSE payload END "
			"WHERE session_id=?",
			-1, &stmt, NULL);
		if (rc == SQLITE_OK) {
			sqlite3_bind_int(stmt, 1, (int)scope);
			sqlite3_bind_int64(stmt, 2, session_id);
			rc = sqlite3_step(stmt);
		}
		sqlite3_finalize(stmt);
		rc = rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
	}
	return memory_store_finish_write(db, rc);
}

/* Additive, idempotent migration. Old rows remain available for audit.
 * Only a preference proven by its original user directive becomes effective;
 * unverified model output remains a saved note. Legacy scope stays local to
 * its session and never outranks a newly explicit preference. */
int memory_store_migrate_preferences(struct db *db, int64_t session_id,
				     int (*verify)(const char *key, const char *value,
						   const char *source))
{
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(
		db->handle,
		"SELECT f.id,f.key_name,f.value_text,COALESCE(f.source_text,'') "
		"FROM memory_facts f JOIN memory_scopes s ON s.session_id=f.session_id "
		"WHERE f.session_id=? AND s.migrated=0 AND f.is_current=1 AND "
		"(f.category='preference' OR f.key_name IN "
		"('preferred_language','response_style','preferred_name')) ORDER BY f.id",
		-1, &stmt, NULL);

	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *key = (const char *)sqlite3_column_text(stmt, 1);
		const char *value = (const char *)sqlite3_column_text(stmt, 2);
		const char *source = (const char *)sqlite3_column_text(stmt, 3);
		int verified = verify && verify(key, value, source);
		int result;

		result = memory_store_add_note(db, session_id, "legacy", key, value, source);
		if (result == 0 && verified) {
			sqlite3_stmt *insert = NULL;

			result = sqlite3_prepare_v2(
				db->handle,
				"INSERT INTO memory_preferences(owner,scope,target,key,value,"
				"source,session_id,event_token,origin) SELECT owner,'session',"
				"CAST(session_id AS TEXT),?,?,?,session_id,?,'legacy' "
				"FROM memory_scopes WHERE session_id=? ON CONFLICT DO NOTHING",
				-1, &insert, NULL);
			if (result == SQLITE_OK) {
				char token[64];

				snprintf(token, sizeof(token), "legacy:%lld",
					 (long long)sqlite3_column_int64(stmt, 0));
				sqlite3_bind_text(insert, 1, preference_key_borrow(key), -1,
						  SQLITE_TRANSIENT);
				sqlite3_bind_text(insert, 2, value, -1, SQLITE_TRANSIENT);
				sqlite3_bind_text(insert, 3, source, -1, SQLITE_TRANSIENT);
				sqlite3_bind_text(insert, 4, token, -1, SQLITE_TRANSIENT);
				sqlite3_bind_int64(insert, 5, session_id);
				result = sqlite3_step(insert);
			}
			sqlite3_finalize(insert);
			result = result == SQLITE_DONE ? 0 : MORPH_ERR_DB;
		}
		if (result != 0) {
			sqlite3_finalize(stmt);
			return result;
		}
	}
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	rc = sqlite3_prepare_v2(db->handle,
				"UPDATE memory_scopes SET migrated=1 WHERE session_id=?", -1, &stmt,
				NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	return 0;
}

int memory_store_event_token(struct db *db, int64_t session_id, const char *event_token, char *out,
			     size_t capacity)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!db || !event_token || !out || !capacity)
		return -EINVAL;
	rc = sqlite3_prepare_v2(
		db->handle,
		"SELECT id FROM model_history_items WHERE session_id=? AND role='user' "
		"AND (turn_id=? OR idempotency_key=?) ORDER BY sequence_no DESC LIMIT 1",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, event_token, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, event_token, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		int bytes = snprintf(out, capacity, "history:%lld",
				     (long long)sqlite3_column_int64(stmt, 0));

		sqlite3_finalize(stmt);
		return bytes < 0 || (size_t)bytes >= capacity ? -ENAMETOOLONG : 1;
	}
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

int memory_store_save_user_summary(struct db *db, int64_t session_id, const char *text)
{
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(
		db->handle,
		"INSERT INTO memory_profiles(session_id,profile_text,updated_at) VALUES(?,?,?) "
		"ON CONFLICT(session_id) DO UPDATE SET "
		"profile_text=excluded.profile_text,updated_at=excluded.updated_at",
		-1, &stmt, NULL);

	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, text, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, (int64_t)time(NULL));
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

int memory_store_add_task(struct db *db, int64_t session_id, const struct memory_task_input *input)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!db || !input || !input->task_type || !input->summary || !input->outcome)
		return -EINVAL;
	rc = sqlite3_prepare_v2(
		db->handle,
		"INSERT INTO "
		"memory_episodes(session_id,task_type,summary_text,outcome_text,success,"
		"entities,key_decisions,artifacts,tools_used,importance,created_at,job_id) "
		"VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT DO NOTHING",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, input->task_type, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, input->summary, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, input->outcome, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 5, !!input->success);
	sqlite3_bind_text(stmt, 6, input->entities ? input->entities : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 7, input->decisions ? input->decisions : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 8, input->artifacts ? input->artifacts : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 9, input->tools ? input->tools : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_double(stmt, 10, input->importance);
	sqlite3_bind_int64(stmt, 11, (int64_t)time(NULL));
	if (input->job_id > 0)
		sqlite3_bind_int64(stmt, 12, input->job_id);
	else
		sqlite3_bind_null(stmt, 12);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static void memory_note_statement_cleanup(sqlite3_stmt **statement)
{
	sqlite3_finalize(*statement);
}

int memory_store_add_note(struct db *db, int64_t session_id, const char *kind, const char *key,
			  const char *value, const char *source)
{
	sqlite3_stmt *stmt MORPH_CLEANUP(memory_note_statement_cleanup) = NULL;
	int rc = sqlite3_prepare_v2(
		db->handle,
		"INSERT INTO memory_candidates(session_id,kind,key,value,source) "
		"VALUES(?,?,?,?,?) ON CONFLICT DO NOTHING",
		-1, &stmt, NULL);

	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, key, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, value, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, source ? source : "", -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	return 0;
}
