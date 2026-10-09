#include "runtime/runtime_internal.h"
#include "runtime/sync.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/file.h"
#include "util/id.h"
#include "util/utf8.h"
#include "blake3.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Import only durable conversation state. Scheduled work, grants, credentials
 * and process state must never become active through a history import. */
static const char *const history_tables[] = {
	"messages", "model_history_items", "react_traces", "outputs",
	"memory_profiles", "memory_facts", "memory_episodes", "memory_procedures",
	"history_compactions", "history_compaction_attempts", "sync_ui_messages",
	"message_attachments", "sub_agent_tasks", "sub_agent_events"
};

#define IMPORT_TABLE_COUNT (sizeof(history_tables) / sizeof(history_tables[0]))

static int import_exec(sqlite3 *db, const char *sql)
{
	if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	return 0;
}

static int import_check(sqlite3 *db, const char *sql)
{
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_FORMAT);
	int step = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (step != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_FORMAT);
	return 0;
}

static int import_has_column(sqlite3 *db, const char *schema, const char *table,
			     const char *column)
{
	return sqlite3_table_column_metadata(db, schema, table, column,
		NULL, NULL, NULL, NULL, NULL) == SQLITE_OK;
}

static int import_revision(sqlite3 *db, int64_t session_id, char hash[65])
{
	blake3_hasher hasher;
	unsigned char digest[BLAKE3_OUT_LEN];
	static const char hex[] = "0123456789abcdef";

	blake3_hasher_init(&hasher);
	for (size_t i = 0; i < IMPORT_TABLE_COUNT; i++) {
		const char *table = history_tables[i];
		morph_buf_t sql;
		sqlite3_stmt *stmt = NULL;
		int step;
		int attachments = strcmp(table, "message_attachments") == 0;
		int tasks = strcmp(table, "sub_agent_tasks") == 0;
		int events = strcmp(table, "sub_agent_events") == 0;
		if (!import_has_column(db, "incoming", table,
			attachments ? "message_id" : tasks ? "parent_session_id" :
			events ? "task_id" : "session_id"))
			continue;
		morph_buf_init(&sql, 128);
		if (tasks)
			morph_buf_puts(&sql, "SELECT * FROM incoming.sub_agent_tasks "
				"WHERE parent_session_id=? ORDER BY task_id");
		else if (events)
			morph_buf_puts(&sql, "SELECT e.* FROM incoming.sub_agent_events e "
				"JOIN incoming.sub_agent_tasks t ON t.task_id=e.task_id "
				"WHERE t.parent_session_id=? ORDER BY e.id");
		else if (attachments)
			morph_buf_puts(&sql, "SELECT a.* FROM incoming.message_attachments a "
				"JOIN incoming.messages m ON m.id=a.message_id "
				"WHERE m.session_id=? ORDER BY a.id");
		else morph_buf_printf(&sql,
			"SELECT * FROM incoming.%s WHERE session_id=? ORDER BY %s",
			table, strcmp(table, "sync_ui_messages") == 0 ? "seq" :
			(import_has_column(db, "incoming", table, "id") ? "id" : "session_id"));
		if (sql.failed) {
			int error = sql.failed;
			morph_buf_cleanup(&sql);
			MORPH_RETURN(error);
		}
		int rc = sqlite3_prepare_v2(db, morph_buf_cstr(&sql), -1, &stmt, NULL);
		morph_buf_cleanup(&sql);
		if (rc != SQLITE_OK)
			MORPH_RETURN(MORPH_ERR_DB);
		sqlite3_bind_int64(stmt, 1, session_id);
		blake3_hasher_update(&hasher, table, strlen(table) + 1);
		while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
			for (int column = 0; column < sqlite3_column_count(stmt); column++) {
				const char *name = sqlite3_column_name(stmt, column);
				/* Ignore local row identities and display-save timestamps. */
				if (strcmp(name, "id") == 0 || strcmp(name, "session_id") == 0 ||
				    strcmp(name, "core_message_id") == 0 ||
				    strcmp(name, "message_id") == 0 ||
				    strcmp(name, "summary_item_id") == 0 ||
				    strcmp(name, "parent_session_id") == 0 ||
				    strcmp(name, "child_session_id") == 0 ||
				    strcmp(name, "created_at") == 0 ||
				    strcmp(name, "updated_at") == 0)
					continue;
				int type = sqlite3_column_type(stmt, column);
				const void *value = sqlite3_column_blob(stmt, column);
				int length = sqlite3_column_bytes(stmt, column);
				blake3_hasher_update(&hasher, name, strlen(name) + 1);
				blake3_hasher_update(&hasher, &type, sizeof(type));
				blake3_hasher_update(&hasher, &length, sizeof(length));
				if (length > 0)
					blake3_hasher_update(&hasher, value, (size_t)length);
			}
		}
		sqlite3_finalize(stmt);
		if (step != SQLITE_DONE)
			MORPH_RETURN(MORPH_ERR_DB);
	}
	blake3_hasher_finalize(&hasher, digest, sizeof(digest));
	for (size_t i = 0; i < sizeof(digest); i++) {
		hash[2 * i] = hex[digest[i] >> 4];
		hash[2 * i + 1] = hex[digest[i] & 15];
	}
	hash[64] = '\0';
	return 0;
}

static int import_session(sqlite3 *db, sqlite3_stmt *source, const char *origin,
			  struct runtime_session_import_result *result)
{
	int64_t old_id = sqlite3_column_int64(source, 0);
	const char *display = (const char *)sqlite3_column_text(source, 1);
	const char *name = (const char *)sqlite3_column_text(source, 2);
	const char *model = (const char *)sqlite3_column_text(source, 3);
	char revision[65];
	morph_buf_t identity;
	morph_buf_t title;
	sqlite3_stmt *stmt = NULL;
	struct db target = { .handle = db };
	struct session session;
	int rc = import_revision(db, old_id, revision);

	if (rc != 0)
		return rc;
	morph_buf_init(&identity, 128);
	morph_buf_init(&title, 128);
	morph_buf_printf(&identity, "%s:%s:%lld", origin, display ? display : "",
		(long long)old_id);
	if (import_has_column(db, "incoming", "sync_session_origins", "origin")) {
		if (sqlite3_prepare_v2(db,
			"SELECT origin FROM incoming.sync_session_origins WHERE session_id=?",
			-1, &stmt, NULL) != SQLITE_OK) {
			MORPH_SET_ERR(rc, MORPH_ERR_DB);
			goto out_free_session;
		}
		sqlite3_bind_int64(stmt, 1, old_id);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			morph_buf_cleanup(&identity);
			morph_buf_init(&identity, 128);
			morph_buf_puts(&identity, (const char *)sqlite3_column_text(stmt, 0));
		}
		sqlite3_finalize(stmt);
		stmt = NULL;
	}
	if (identity.failed) {
		MORPH_SET_ERR(rc, identity.failed);
		goto out_free_session;
	}
	if (sqlite3_prepare_v2(db,
		"SELECT session_id FROM sync_import_receipts WHERE origin=? AND revision=?",
		-1, &stmt, NULL) != SQLITE_OK) {
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
		goto out_free_session;
	}
	sqlite3_bind_text(stmt, 1, morph_buf_cstr(&identity), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, revision, -1, SQLITE_TRANSIENT);
	int step = sqlite3_step(stmt);
	int64_t previous_id = step == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (step == SQLITE_ROW) {
		if (sqlite3_prepare_v2(db,
			"INSERT INTO import_session_map(old_id,new_id,is_new) "
			"SELECT ?,id,0 FROM sessions WHERE id=?", -1, &stmt, NULL) != SQLITE_OK) {
			MORPH_SET_ERR(rc, MORPH_ERR_DB);
			goto out_free_session;
		}
		sqlite3_bind_int64(stmt, 1, old_id);
		sqlite3_bind_int64(stmt, 2, previous_id);
		if (sqlite3_step(stmt) != SQLITE_DONE) {
			MORPH_SET_ERR(rc, MORPH_ERR_DB);
			goto out_free_session;
		}
		result->unchanged++;
		goto out_free_session;
	}
	if (step != SQLITE_DONE) {
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
		goto out_free_session;
	}
	char *base = utf8_dup_clamped(name ? name : "Imported conversation", 180);
	if (!base) {
		MORPH_SET_ERR(rc, -ENOMEM);
		goto out_free_session;
	}
	morph_buf_puts(&title, base);
	for (int suffix = 1; session_get_by_name(&target, morph_buf_cstr(&title), &session) == 0;
	     suffix++) {
		morph_buf_cleanup(&title);
		morph_buf_init(&title, 128);
		morph_buf_printf(&title, "%s [sync %d]", base, suffix);
	}
	free(base);
	if (title.failed) {
		MORPH_SET_ERR(rc, title.failed);
		goto out_free_session;
	}
	rc = session_create(&target, morph_buf_cstr(&title), model, &session);
	if (rc != 0)
		goto out_free_session;
	if (sqlite3_prepare_v2(db,
		"INSERT INTO import_session_map(old_id,new_id) VALUES(?,?)",
		-1, &stmt, NULL) != SQLITE_OK) {
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
		goto out_free_session;
	}
	sqlite3_bind_int64(stmt, 1, old_id);
	sqlite3_bind_int64(stmt, 2, session.id);
	if (sqlite3_step(stmt) != SQLITE_DONE)
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (rc != 0)
		goto out_free_session;
	if (sqlite3_prepare_v2(db,
		"INSERT INTO sync_import_receipts(origin,revision,session_id) VALUES(?,?,?)",
		-1, &stmt, NULL) != SQLITE_OK) {
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
		goto out_free_session;
	}
	sqlite3_bind_text(stmt, 1, morph_buf_cstr(&identity), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, revision, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, session.id);
	if (sqlite3_step(stmt) != SQLITE_DONE)
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (rc == 0 && sqlite3_prepare_v2(db,
		"INSERT INTO sync_session_origins(session_id,origin) VALUES(?,?)",
		-1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, session.id);
		sqlite3_bind_text(stmt, 2, morph_buf_cstr(&identity), -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) != SQLITE_DONE)
			MORPH_SET_ERR(rc, MORPH_ERR_DB);
	} else {
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
	}
	if (rc == 0)
		result->imported++;

out_free_session:
	sqlite3_finalize(stmt);
	morph_buf_cleanup(&identity);
	morph_buf_cleanup(&title);
	MORPH_RETURN(rc);
}

static int64_t import_offset(sqlite3 *db, const char *table)
{
	morph_buf_t sql;
	sqlite3_stmt *stmt = NULL;
	int64_t value = -1;
	if (!import_has_column(db, "main", table, "id"))
		return 0;
	morph_buf_init(&sql, 128);
	morph_buf_printf(&sql, "SELECT COALESCE(MAX(id),0) FROM main.%s", table);
	if (!sql.failed && sqlite3_prepare_v2(db, morph_buf_cstr(&sql), -1,
		&stmt, NULL) == SQLITE_OK &&
	    sqlite3_step(stmt) == SQLITE_ROW)
		value = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	morph_buf_cleanup(&sql);
	return value;
}

static int import_table(sqlite3 *db, const char *table, int64_t message_offset,
			int64_t model_offset)
{
	morph_buf_t sql;
	morph_buf_t columns;
	morph_buf_t values;
	sqlite3_stmt *stmt = NULL;
	int64_t offset = import_offset(db, table);
	int rc = 0;
	int count = 0;
	int step;

	if (!import_has_column(db, "incoming", table, "session_id"))
		return 0;
	if (offset < 0)
		MORPH_RETURN(MORPH_ERR_DB);
	morph_buf_init(&sql, 128);
	morph_buf_init(&columns, 128);
	morph_buf_init(&values, 128);
	morph_buf_printf(&sql, "PRAGMA main.table_info(%s)", table);
	if (sql.failed || sqlite3_prepare_v2(db, morph_buf_cstr(&sql), -1,
		&stmt, NULL) != SQLITE_OK) {
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
		goto out_free_columns;
	}
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *name = (const char *)sqlite3_column_text(stmt, 1);
		if (!import_has_column(db, "incoming", table, name))
			continue;
		if (count++) {
			morph_buf_putc(&columns, ',');
			morph_buf_putc(&values, ',');
		}
		morph_buf_printf(&columns, "\"%s\"", name);
		if (strcmp(name, "session_id") == 0)
			morph_buf_puts(&values, "m.new_id");
		else if (strcmp(name, "id") == 0 || strcmp(name, "superseded_by") == 0)
			morph_buf_printf(&values, "s.\"%s\"+%lld", name, (long long)offset);
		else if (strcmp(name, "core_message_id") == 0)
			morph_buf_printf(&values, "s.core_message_id+%lld",
				(long long)message_offset);
		else if (strcmp(name, "summary_item_id") == 0)
			morph_buf_printf(&values, "s.summary_item_id+%lld",
				(long long)model_offset);
		else
			morph_buf_printf(&values, "s.\"%s\"", name);
	}
	if (step != SQLITE_DONE || count == 0 || columns.failed || values.failed) {
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
		goto out_free_columns;
	}
	morph_buf_cleanup(&sql);
	morph_buf_init(&sql, 128);
	morph_buf_printf(&sql,
		"INSERT INTO main.%s(%s) SELECT %s FROM incoming.%s s "
		"JOIN import_session_map m ON s.session_id=m.old_id WHERE m.is_new=1",
		table, morph_buf_cstr(&columns), morph_buf_cstr(&values), table);
	rc = sql.failed ? sql.failed : import_exec(db, morph_buf_cstr(&sql));

out_free_columns:
	sqlite3_finalize(stmt);
	morph_buf_cleanup(&sql);
	morph_buf_cleanup(&columns);
	morph_buf_cleanup(&values);
	MORPH_RETURN(rc);
}

int runtime_session_import_file(struct runtime *runtime, const char *path,
	const char *origin, struct runtime_session_import_result *result)
{
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	int rc;
	int step;
	int transaction = 0;

	if (!runtime || !path || !origin || !result)
		MORPH_RETURN(-EINVAL);
	memset(result, 0, sizeof(*result));
	if (!file_exists(path))
		MORPH_RETURN(-ENOENT);
	rc = pthread_mutex_trylock(&runtime->context.execution_lock);
	if (rc != 0)
		MORPH_RETURN(-rc);
	if (sqlite3_open_v2(sqlite3_db_filename(runtime->context.database.handle, "main"),
		&db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
		goto out_close_import;
	}
	sqlite3_busy_timeout(db, 5000);
	(void)sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, NULL);
	if (sqlite3_prepare_v2(db, "ATTACH DATABASE ? AS incoming", -1, &stmt, NULL) != SQLITE_OK) {
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
		goto out_close_import;
	}
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	step = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (step != SQLITE_DONE || !import_has_column(db, "incoming", "sessions", "display_id") ||
	    !import_has_column(db, "incoming", "messages", "session_id")) {
		MORPH_SET_ERR(rc, MORPH_ERR_FORMAT);
		goto out_close_import;
	}
	rc = import_check(db, "PRAGMA incoming.foreign_key_check");
	if (rc != 0)
		goto out_close_import;
	rc = import_check(db,
		"SELECT 1 FROM incoming.messages m LEFT JOIN incoming.sessions s "
		"ON s.id=m.session_id WHERE s.id IS NULL LIMIT 1");
	if (rc != 0)
		goto out_close_import;
	if (import_has_column(db, "incoming", "schema_migrations", "version")) {
		rc = import_check(db,
			"SELECT 1 WHERE (SELECT COALESCE(MAX(version),0) FROM "
			"incoming.schema_migrations) "
			"> (SELECT COALESCE(MAX(version),0) FROM main.schema_migrations)");
		if (rc != 0)
			goto out_close_import;
	}
	rc = import_exec(db, "PRAGMA foreign_keys=ON; BEGIN IMMEDIATE;");
	if (rc != 0)
		goto out_close_import;
	transaction = 1;
	rc = import_exec(db,
		"CREATE TABLE IF NOT EXISTS sync_import_receipts("
		"origin TEXT NOT NULL,revision TEXT NOT NULL,session_id INTEGER,"
		"PRIMARY KEY(origin,revision));"
		"CREATE TABLE IF NOT EXISTS sync_session_origins("
		"session_id INTEGER PRIMARY KEY,origin TEXT NOT NULL);"
		"CREATE TEMP TABLE import_session_map(old_id INTEGER PRIMARY KEY,new_id "
		"INTEGER,is_new INTEGER DEFAULT 1);"
		"CREATE TABLE IF NOT EXISTS sync_ui_messages(session_id INTEGER,turn_id TEXT,"
		"core_message_id INTEGER,seq INTEGER,type TEXT,content TEXT,attachments_json TEXT,"
		"structured_data TEXT,agent_ui_ir TEXT,hitl_verdict INTEGER,created_at "
		"INTEGER,updated_at INTEGER);");
	if (rc != 0)
		goto out_close_import;
	if (sqlite3_prepare_v2(db,
		"SELECT id,display_id,name,model FROM incoming.sessions ORDER BY id",
		-1, &stmt, NULL) != SQLITE_OK) {
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
		goto out_close_import;
	}
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		rc = import_session(db, stmt, origin, result);
		if (rc != 0)
			break;
	}
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (rc == 0 && step != SQLITE_DONE)
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
	int64_t message_offset = import_offset(db, "messages");
	int64_t model_offset = import_offset(db, "model_history_items");
	if (message_offset < 0 || model_offset < 0)
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
	if (rc == 0)
		rc = import_exec(db, "UPDATE sessions SET (created_at,updated_at,token_used)="
			"(SELECT s.created_at,s.updated_at,s.token_used FROM incoming.sessions s "
			"JOIN import_session_map m ON m.old_id=s.id WHERE m.new_id=sessions.id) "
			"WHERE id IN (SELECT new_id FROM import_session_map WHERE is_new=1)");
	for (size_t i = 0; rc == 0 && i < IMPORT_TABLE_COUNT; i++)
		rc = import_table(db, history_tables[i], message_offset, model_offset);
	if (rc == 0 && import_has_column(db, "incoming", "message_attachments", "message_id")) {
		morph_buf_t sql;
		morph_buf_init(&sql, 128);
		morph_buf_printf(&sql,
			"INSERT INTO message_attachments(message_id,kind,path,sha256) "
			"SELECT a.message_id+%lld,a.kind,a.path,a.sha256 "
			"FROM incoming.message_attachments a JOIN incoming.messages s ON "
			"s.id=a.message_id "
			"JOIN import_session_map m ON m.old_id=s.session_id WHERE m.is_new=1",
			(long long)message_offset);
		rc = sql.failed ? sql.failed : import_exec(db, morph_buf_cstr(&sql));
		morph_buf_cleanup(&sql);
	}
	if (rc == 0 && import_has_column(db, "incoming", "sub_agent_tasks", "task_id"))
		rc = import_exec(db,
			"CREATE TEMP TABLE import_task_map AS "
			"SELECT t.task_id AS old_id,CASE WHEN EXISTS(SELECT 1 FROM "
			"main.sub_agent_tasks x "
			"WHERE x.task_id=t.task_id) THEN t.task_id||'@'||p.new_id ELSE "
			"t.task_id END AS new_id "
			"FROM incoming.sub_agent_tasks t JOIN import_session_map p ON "
			"p.old_id=t.parent_session_id "
			"WHERE p.is_new=1;"
			"INSERT INTO "
			"sub_agent_tasks(task_id,parent_session_id,child_session_id,agent_name,"
			"description,mode,status,result,error_code,iterations,started_at,ended_at) "
			"SELECT "
			"m.new_id,p.new_id,c.new_id,t.agent_name,t.description,t.mode,"
			"t.status,t.result,"
			"t.error_code,t.iterations,t.started_at,t.ended_at FROM "
			"incoming.sub_agent_tasks t "
			"JOIN import_task_map m ON t.task_id=m.old_id "
			"JOIN import_session_map p ON p.old_id=t.parent_session_id "
			"LEFT JOIN import_session_map c ON c.old_id=t.child_session_id;"
			"INSERT INTO sub_agent_events(task_id,event_json,created_at) "
			"SELECT m.new_id,e.event_json,e.created_at FROM "
			"incoming.sub_agent_events e "
			"JOIN import_task_map m ON e.task_id=m.old_id;");
	if (rc == 0)
		rc = import_check(db, "PRAGMA main.foreign_key_check");
	if (rc == 0)
		rc = import_exec(db, "COMMIT");
	if (rc == 0)
		transaction = 0;

out_close_import:
	sqlite3_finalize(stmt);
	if (transaction)
		(void)import_exec(db, "ROLLBACK");
	if (db)
		sqlite3_close(db);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	if (rc != 0)
		memset(result, 0, sizeof(*result));
	MORPH_RETURN(rc);
}

int runtime_sync_import_backup(struct runtime *runtime,
	const struct morph_sync_config *cfg, const char *snapshot_id,
	struct runtime_session_import_result *result)
{
	struct morph_sync_backup *backups = NULL;
	char origin[MORPH_SYNC_DEVICE_ID_MAX] = { 0 };
	char directory[PATH_MAX];
	char path[PATH_MAX];
	char token[64];
	int count = 0;
	int rc;

	if (!runtime || !cfg || !snapshot_id || !result)
		MORPH_RETURN(-EINVAL);
	memset(result, 0, sizeof(*result));
	rc = morph_sync_backups(cfg, "data.db", &backups, &count);
	if (rc != 0)
		return rc;
	for (int i = 0; i < count; i++) {
		if (strcmp(backups[i].snapshot_id, snapshot_id) == 0)
			strncpy(origin, backups[i].device_id, sizeof(origin) - 1);
	}
	morph_sync_backups_free(backups);
	if (!origin[0])
		MORPH_RETURN(-ENOENT);
	rc = file_path_join(directory, sizeof(directory), cfg->sync_dir, ".morph-sync/import");
	if (rc == 0)
		rc = file_ensure_dir(directory);
	if (rc == 0)
		rc = morph_random_id("session_", token, sizeof(token));
	if (rc == 0)
		rc = file_path_join(path, sizeof(path), directory, token);
	if (rc != 0)
		return rc;
	rc = morph_sync_restore_db(cfg, snapshot_id, path);
	if (rc == 0)
		rc = runtime_session_import_file(runtime, path, origin, result);
	(void)unlink(path);
	MORPH_RETURN(rc);
}
