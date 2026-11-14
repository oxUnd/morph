#include "preferences.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/random.h"
#include "nb/encoding.h"
#include "nb/utf8.h"
#include "nb/str.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

const char *preference_key_borrow(const char *key)
{
	if (!key)
		return NULL;
	if (!strcmp(key, "preferred_language") || !strcmp(key, "language") ||
	    !strcmp(key, "response_language") || !strcmp(key, "language_preference") ||
	    !strcmp(key, "preferred_response_language"))
		return "response.language";
	if (!strcmp(key, "response_style") || !strcmp(key, "style") ||
	    !strcmp(key, "verbosity") || !strcmp(key, "detail_level"))
		return "response.detail";
	if (!strcmp(key, "preferred_name"))
		return "user.preferred_name";
	return key;
}

int preference_bind(struct db *db, int64_t session_id,
		    const char *owner, const char *project)
{
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!db || !db->handle || session_id <= 0 || !owner || !*owner)
		return NB_ERR_INVAL;
	rc = db_lock_acquire(db, &lock);
	if (rc != 0)
		return rc;
	rc = sqlite3_prepare_v2(
		db->handle,
		"INSERT INTO memory_scopes(session_id,owner,project) VALUES(?,?,?) "
		"ON CONFLICT(session_id) DO UPDATE SET project=excluded.project "
		"WHERE memory_scopes.owner=excluded.owner AND "
		"(memory_scopes.project='' OR memory_scopes.project=excluded.project)",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, owner, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, project ? project : "", -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	if (!sqlite3_changes(db->handle))
		return NB_ERR_NOT_PERMITTED;
	return 0;
}

static int preference_valid_key(const char *key)
{
	return key && strlen(key) <= 128 && nb_str_is_alnum(nb_str_from_cstr(key), "._-");
}

int preference_set(struct db *db, int64_t session_id, const char *scope,
		   const char *key, const char *value, const char *source,
		   const char *event_token)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;

	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	sqlite3_stmt *stmt = NULL;
	char token[64];
	int rc;

	key = preference_key_borrow(key);
	if (!db || !db->handle || !preference_valid_key(key) || !scope ||
	    (strcmp(scope, "personal") && strcmp(scope, "project") && strcmp(scope, "session")) ||
	    (value &&
	     (!*value || strlen(value) > 1024 || nb_utf8_validate(nb_str_from_cstr(value), NULL))))
		return NB_ERR_INVAL;
	if (value && !strcmp(key, "response.language")) {
		if (!strcmp(value, "zh") || !strcmp(value, "中文"))
			value = "Chinese";
		else if (!strcmp(value, "en") || !strcmp(value, "英文"))
			value = "English";
	}
	if (!event_token || !*event_token) {
		rc = ((file_status_1 = ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
					: !(nb_buf_clear(&file_value_1),
					    nb_buf_append_cstr(&file_value_1, "pref_"))
						? NB_ERR_NOMEM
						: nb_random_hex(&file_value_1, 16, NULL))) != NB_OK
			      ? file_status_1
		      : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), token,
							sizeof(token))) == NB_ERR_RANGE
			      ? NB_ERR_NO_SPACE
			      : file_status_1);
		if (rc != 0)
			return rc;
		event_token = token;
	}
	rc = db_lock_acquire(db, &lock);
	if (rc != 0)
		return rc;
	rc = sqlite3_prepare_v2(db->handle,
		"SELECT 1 FROM memory_scopes WHERE session_id=? "
		"AND (?!='project' OR project!='')", -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, scope, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (rc != SQLITE_ROW)
		return rc == SQLITE_DONE ? NB_ERR_NOT_FOUND : MORPH_ERR_DB;
	/* One atomic append is the update. The latest revision per semantic key
	 * is authoritative; NULL is a tombstone, not permission to resurrect it.
	 * Only input ingestion / explicit commands call this API, never jobs. */
	rc = sqlite3_prepare_v2(db->handle,
		"INSERT INTO memory_preferences(owner,scope,target,key,value,source,"
		"session_id,event_token,source_order) SELECT owner,?1,CASE ?1 "
		"WHEN 'personal' THEN '' WHEN 'project' THEN project "
		"ELSE CAST(session_id AS TEXT) END,?2,?3,?4,session_id,?5, "
		"COALESCE((SELECT id FROM model_history_items WHERE session_id=?6 "
		"AND role='user' AND ?5='history:'||id), "
		"(SELECT COALESCE(MAX(id),0)+1 FROM model_history_items)) "
		"FROM memory_scopes WHERE session_id=?6 "
		"AND (?1!='project' OR project!='') "
		"ON CONFLICT DO NOTHING", -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, scope, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);
	if (value)
		sqlite3_bind_text(stmt, 3, value, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 3);
	sqlite3_bind_text(stmt, 4, source ? source : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, event_token, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 6, session_id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	if (!sqlite3_changes(db->handle)) {
		rc = sqlite3_prepare_v2(db->handle,
			"SELECT p.value IS ? FROM memory_preferences p "
			"JOIN memory_scopes s ON s.owner=p.owner WHERE s.session_id=? "
			"AND p.session_id=s.session_id AND p.event_token=? AND p.scope=? "
			"AND p.key=? AND p.target=CASE p.scope WHEN 'personal' THEN '' "
			"WHEN 'project' THEN s.project ELSE CAST(s.session_id AS TEXT) END",
			-1, &stmt, NULL);
		if (rc != SQLITE_OK)
			return MORPH_ERR_DB;
		if (value)
			sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);
		else
			sqlite3_bind_null(stmt, 1);
		sqlite3_bind_int64(stmt, 2, session_id);
		sqlite3_bind_text(stmt, 3, event_token, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, scope, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, key, -1, SQLITE_TRANSIENT);
		rc = sqlite3_step(stmt);
		int same = rc == SQLITE_ROW && sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
		if (!same)
			return NB_ERR_STALE;
	}
	return 0;
}

static const char *preference_query_borrow(int history)
{
	return history ?
		"SELECT p.key,p.value,p.scope,p.id,p.source FROM memory_preferences p "
		"JOIN memory_scopes s ON s.owner=p.owner WHERE s.session_id=? "
		"AND (p.scope='personal' OR (p.scope='project' AND p.target=s.project) "
		"OR (p.scope='session' AND p.target=CAST(s.session_id AS TEXT))) "
		"ORDER BY p.id DESC" :
		"WITH versions AS (SELECT p.*,ROW_NUMBER() OVER "
		"(PARTITION BY p.scope,p.target,p.key ORDER BY p.source_order DESC,p.id DESC) AS v "
		"FROM memory_preferences p JOIN memory_scopes s ON s.owner=p.owner "
		"WHERE s.session_id=? AND (p.scope='personal' OR "
		"(p.scope='project' AND p.target=s.project) OR "
		"(p.scope='session' AND p.target=CAST(s.session_id AS TEXT)))), "
		"resolved AS (SELECT *,ROW_NUMBER() OVER (PARTITION BY key ORDER BY "
		"CASE origin WHEN 'explicit' THEN 1 ELSE 0 END DESC, "
		"CASE scope WHEN 'session' THEN 3 WHEN 'project' THEN 2 ELSE 1 END DESC) "
		"AS rank FROM versions WHERE v=1 AND value IS NOT NULL) "
		"SELECT key,value,scope,id,source FROM resolved WHERE rank=1 ORDER BY CASE key "
		"WHEN 'response.language' THEN 0 WHEN 'response.detail' THEN 1 ELSE 2 END,key";
}

static void preference_statement_cleanup(sqlite3_stmt **owner)
{
	if (*owner)
		sqlite3_finalize(*owner);
	*owner = NULL;
}

int preference_visit_effective(struct db *db, int64_t session_id,
			       preference_visit_fn visit, void *data)
{
	sqlite3_stmt *stmt NB_CLEANUP(preference_statement_cleanup) = NULL;
	int rc;

	if (!db || !db->handle || !visit)
		return NB_ERR_INVAL;
	rc = sqlite3_prepare_v2(db->handle, preference_query_borrow(0), -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	int error = 0;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		error = visit((const char *)sqlite3_column_text(stmt, 0),
			      (const char *)sqlite3_column_text(stmt, 1),
			      (const char *)sqlite3_column_text(stmt, 2),
			      sqlite3_column_int64(stmt, 3), data);
		if (error != 0)
			break;
	}
	return error ? error : rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

char *preference_render_copy(struct db *db, int64_t session_id, int history)
{
	const char *sql = preference_query_borrow(history);
	sqlite3_stmt *stmt = NULL;
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc;

	if (!db || !db->handle ||
	    (nb_buf_init(&buf), nb_buf_reserve(&buf, 512) ? 0 : NB_ERR_NOMEM) != 0)
		return NULL;
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, session_id);
		while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
			const char *key = (const char *)sqlite3_column_text(stmt, 0);
			const char *value = (const char *)sqlite3_column_text(stmt, 1);
			const char *scope = (const char *)sqlite3_column_text(stmt, 2);
			const char *source = (const char *)sqlite3_column_text(stmt, 4);

			if ((nb_buf_printf(&buf, "- %s: %s [%s; revision=%lld]\n", key,
					   value ? value : "(unset)", scope,
					   (long long)sqlite3_column_int64(stmt, 3))
				     ? 0
				     : NB_ERR_NOMEM) != 0) {
				rc = SQLITE_NOMEM;
				break;
			}
			if (history && (nb_buf_printf(&buf, "  source: %s\n", source ? source : "")
						? 0
						: NB_ERR_NOMEM) != 0) {
				rc = SQLITE_NOMEM;
				break;
			}
		}
	}
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE) {
		nb_buf_cleanup(&buf);
		return NULL;
	}
	return (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
}

int preference_clear_session(struct db *db, int64_t session_id)
{
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db->handle,
		"INSERT INTO memory_preferences(owner,scope,target,key,value,source,"
		"session_id,event_token,source_order) SELECT owner,scope,target,key,NULL,"
		"'session memory cleared',?1,lower(hex(randomblob(16))), "
		"(SELECT COALESCE(MAX(id),0)+1 FROM model_history_items) "
		"FROM memory_preferences WHERE scope='session' "
		"AND target=CAST(?1 AS TEXT) GROUP BY owner,scope,target,key",
		-1, &stmt, NULL);

	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	return 0;
}
