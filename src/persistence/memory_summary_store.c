#include "memory_summary_store.h"

#include "memory_repository.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"

#include <errno.h>
#include <string.h>

static void memory_summary_statement_cleanup(sqlite3_stmt **statement)
{
	sqlite3_finalize(*statement);
}

int memory_summary_revision(struct db *db, int64_t session_id, int64_t *revision)
{
	sqlite3_stmt *stmt NB_CLEANUP(memory_summary_statement_cleanup) = NULL;

	if (!db || !db->handle || !revision)
		return NB_ERR_INVAL;
	if (sqlite3_prepare_v2(db->handle,
			       "SELECT content_revision FROM memory_scopes WHERE session_id=?", -1,
			       &stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	int step = sqlite3_step(stmt);

	if (step != SQLITE_ROW)
		return step == SQLITE_DONE ? NB_ERR_NOT_FOUND : MORPH_ERR_DB;
	*revision = sqlite3_column_int64(stmt, 0);
	return 0;
}

int memory_summary_save(struct db *db, int64_t session_id, int64_t version, int64_t revision,
			const char *title, const char *text, const char *sources)
{
	sqlite3_stmt *stmt NB_CLEANUP(memory_summary_statement_cleanup) = NULL;
	int64_t current = 0;
	int rc = memory_summary_revision(db, session_id, &current);

	if (rc != 0)
		return rc;
	if (revision != current)
		return NB_ERR_STALE;
	if (sqlite3_prepare_v2(
		    db->handle,
		    "INSERT INTO memory_summaries(session_id,memory_version,content_revision,title,"
		    "text,sources) VALUES(?,?,?,?,?,?) ON CONFLICT(session_id) DO UPDATE SET "
		    "memory_version=excluded.memory_version,content_revision=excluded.content_"
		    "revision,"
		    "title=excluded.title,text=excluded.text,sources=excluded.sources,"
		    "updated_at=CAST(strftime('%s','now') AS INTEGER)",
		    -1, &stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_int64(stmt, 2, version);
	sqlite3_bind_int64(stmt, 3, revision);
	sqlite3_bind_text(stmt, 4, title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, text, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, sources, -1, SQLITE_TRANSIENT);
	return sqlite3_step(stmt) == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

int memory_note_save(struct db *db, int64_t session_id, const char *title, const char *text,
		     const char *source, const char *event_token, const char *scope)
{
	sqlite3_stmt *stmt NB_CLEANUP(memory_summary_statement_cleanup) = NULL;

	if (!db || !db->handle || !title || !text || !source || !event_token || !scope ||
	    (strcmp(scope, "project") && strcmp(scope, "session")))
		return NB_ERR_INVAL;
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	int rc = db_lock_acquire(db, &lock);

	if (rc != 0)
		return rc;
	if (sqlite3_prepare_v2(
		    db->handle,
		    "INSERT INTO memory_notes(session_id,title,text,source,event_token,scope) "
		    "VALUES(?,?,?,?,?,?) ON CONFLICT(session_id,event_token,title) DO NOTHING",
		    -1, &stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, text, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, source, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, event_token, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, scope, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) != SQLITE_DONE)
		return MORPH_ERR_DB;
	if (sqlite3_changes(db->handle) == 1)
		return 0;
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (sqlite3_prepare_v2(
		    db->handle,
		    "SELECT 1 FROM memory_notes WHERE session_id=? AND event_token=? AND title=? "
		    "AND text=? AND source=? AND scope=?",
		    -1, &stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, event_token, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, text, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, source, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, scope, -1, SQLITE_TRANSIENT);
	int step = sqlite3_step(stmt);

	return step == SQLITE_ROW ? 0 : step == SQLITE_DONE ? NB_ERR_STATE : MORPH_ERR_DB;
}

static int memory_record_change(struct db *db, int64_t session_id, const char *kind, int64_t id,
				const char *expected, const char *replacement, const char *source)
{
	const char *table = NULL;
	const char *value = NULL;
	const char *source_column = NULL;
	const char *time_column = "updated_at";
	sqlite3_stmt *stmt NB_CLEANUP(memory_summary_statement_cleanup) = NULL;
	nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};

	if (!strcmp(kind, "notes")) {
		table = "memory_notes";
		value = "text";
		source_column = "source";
	} else if (!strcmp(kind, "facts")) {
		table = "memory_facts";
		value = "value_text";
		source_column = "source_text";
	} else if (!strcmp(kind, "learned_facts") || !strcmp(kind, "tips") ||
		   !strcmp(kind, "unapplied_settings")) {
		table = "memory_candidates";
		value = "value";
		time_column = "created_at";
		source_column = "source";
	} else if (!strcmp(kind, "past_tasks") && !replacement) {
		table = "memory_episodes";
		value = "summary_text";
	} else if (!strcmp(kind, "unapplied_rules") && !replacement) {
		table = "memory_procedures";
		value = "rule_text";
	} else {
		return NB_ERR_INVAL;
	}
	int rc = (nb_buf_init(&sql), nb_buf_reserve(&sql, 256) ? 0 : NB_ERR_NOMEM);

	if (rc == 0 && replacement)
		rc = (nb_buf_printf(&sql,
				    "UPDATE %s SET %s=?4,%s=?5,%s=CAST(strftime('%%s','now') AS "
				    "INTEGER) WHERE "
				    "id=?1 AND session_id=?2 AND %s=?3",
				    table, value, source_column, time_column, value)
			      ? 0
			      : NB_ERR_NOMEM);
	else if (rc == 0)
		rc = (nb_buf_printf(&sql, "DELETE FROM %s WHERE id=?1 AND session_id=?2 AND %s=?3",
				    table, value)
			      ? 0
			      : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	if (sqlite3_prepare_v2(db->handle, nb_buf_cstr(&sql), -1, &stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, session_id);
	sqlite3_bind_text(stmt, 3, expected, -1, SQLITE_TRANSIENT);
	if (replacement) {
		sqlite3_bind_text(stmt, 4, replacement, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, source, -1, SQLITE_TRANSIENT);
	}
	if (sqlite3_step(stmt) != SQLITE_DONE)
		return MORPH_ERR_DB;
	return sqlite3_changes(db->handle) == 1 ? 0 : NB_ERR_STALE;
}

int memory_record_edit(struct db *db, int64_t session_id, const char *kind, int64_t id,
		       int64_t version, const char *expected_value, const char *replacement,
		       const char *source)
{
	sqlite3_stmt *stmt NB_CLEANUP(memory_summary_statement_cleanup) = NULL;
	if (!kind || !expected_value || !source || id <= 0 || version < 0)
		return NB_ERR_INVAL;
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	int rc = db_lock_acquire(db, &lock);

	if (rc != 0)
		return rc;
	rc = memory_store_begin_versioned_write(db, session_id, version);
	if (rc != 0)
		return rc;
	rc = memory_record_change(db, session_id, kind, id, expected_value, replacement, source);
	if (rc == 0) {
		if (sqlite3_prepare_v2(
			    db->handle,
			    "UPDATE memory_scopes SET generation=generation+1 WHERE session_id=?",
			    -1, &stmt, NULL) != SQLITE_OK)
			rc = MORPH_ERR_DB;
		else {
			sqlite3_bind_int64(stmt, 1, session_id);
			rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : MORPH_ERR_DB;
		}
	}
	return memory_store_finish_write(db, rc);
}
