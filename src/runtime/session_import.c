#include "runtime/runtime_internal.h"
#include "nb/str.h"
#include "db/error_migration.h"
#include "nb/cleanup.h"
#include "runtime/sync.h"
#include "nb/buf.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/random.h"
#include "nb/encoding.h"
#include "nb/utf8.h"
#include "blake3.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Import only durable conversation state. Scheduled work, grants, credentials
 * and process state must never become active through a history import. */
static const char *const history_tables[] = {"messages",
					     "model_history_items",
					     "react_traces",
					     "outputs",
					     "memory_profiles",
					     "memory_facts",
					     "memory_episodes",
					     "memory_procedures",
					     "history_compactions",
					     "history_compaction_attempts",
					     "sync_ui_messages",
					     "message_attachments",
					     "sub_agent_tasks",
					     "sub_agent_events",
					     "session_plans", "session_skill_activations",
					     "agent_threads", "agent_mailbox", "agent_events"};

#define IMPORT_TABLE_COUNT (sizeof(history_tables) / sizeof(history_tables[0]))

static int import_exec(sqlite3 *db, const char *sql)
{
	if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	return 0;
}

static int import_check(sqlite3 *db, const char *sql)
{
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
		return NB_ERR_FORMAT;
	int step = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (step != SQLITE_DONE)
		return NB_ERR_FORMAT;
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

	blake3_hasher_init(&hasher);
	for (size_t i = 0; i < IMPORT_TABLE_COUNT; i++) {
		const char *table = history_tables[i];
		nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};
		sqlite3_stmt *stmt = NULL;
		int step;
		int attachments = strcmp(table, "message_attachments") == 0;
		int tasks = strcmp(table, "sub_agent_tasks") == 0;
		int events = strcmp(table, "sub_agent_events") == 0;
		int agents = !strcmp(table, "agent_threads") || !strcmp(table, "agent_mailbox") ||
			!strcmp(table, "agent_events");
		if (!import_has_column(db, "incoming", table,
			agents ? "root_session_id" : attachments ? "message_id" : tasks ? "parent_session_id" :
			events ? "task_id" : "session_id"))
			continue;
		(nb_buf_init(&sql), nb_buf_reserve(&sql, 128));
		if (agents)
			nb_buf_printf(&sql,
				      "SELECT * FROM incoming.%s WHERE root_session_id=? "
				      "ORDER BY %s",
				      table, !strcmp(table, "agent_threads") ? "agent_id" : "id");
		else if (tasks)
			nb_buf_append_cstr(&sql, "SELECT * FROM incoming.sub_agent_tasks "
						 "WHERE parent_session_id=? ORDER BY task_id");
		else if (events)
			nb_buf_append_cstr(&sql,
					   "SELECT e.* FROM incoming.sub_agent_events e "
					   "JOIN incoming.sub_agent_tasks t ON t.task_id=e.task_id "
					   "WHERE t.parent_session_id=? ORDER BY e.id");
		else if (attachments)
			nb_buf_append_cstr(&sql, "SELECT a.* FROM incoming.message_attachments a "
						 "JOIN incoming.messages m ON m.id=a.message_id "
						 "WHERE m.session_id=? ORDER BY a.id");
		else
			(void)(nb_buf_printf(
				       &sql,
				       "SELECT * FROM incoming.%s WHERE session_id=? ORDER BY %s",
				       table,
				       !strcmp(table, "session_skill_activations") ? "skill_name"
				       : strcmp(table, "sync_ui_messages") == 0
					       ? "seq"
					       : (import_has_column(db, "incoming", table, "id")
							  ? "id"
							  : "session_id"))
				       ? 0
				       : NB_ERR_NOMEM);
		if (nb_buf_status(&sql)) {
			int error = nb_buf_status(&sql);
			nb_buf_cleanup(&sql);
			return error;
		}
		int rc = sqlite3_prepare_v2(db, nb_buf_cstr(&sql), -1, &stmt, NULL);
		nb_buf_cleanup(&sql);
		if (rc != SQLITE_OK)
			return MORPH_ERR_DB;
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
			return MORPH_ERR_DB;
	}
	blake3_hasher_finalize(&hasher, digest, sizeof(digest));
	(void)nb_hex_encode_to(digest, sizeof(digest), false, hash, 65, NULL);
	return 0;
}

static void import_stmt_cleanup(sqlite3_stmt **statement)
{
	sqlite3_finalize(*statement);
	*statement = NULL;
}

static int import_legacy_error_schema(sqlite3 *db, bool *legacy)
{
	sqlite3_stmt *statement NB_CLEANUP(import_stmt_cleanup) = NULL;

	*legacy = true;
	if (!import_has_column(db, "incoming", "schema_migrations", "version"))
		return 0;
	if (sqlite3_prepare_v2(db, "SELECT 1 FROM incoming.schema_migrations WHERE version=3", -1,
			       &statement, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	int step = sqlite3_step(statement);
	if (step != SQLITE_ROW && step != SQLITE_DONE)
		return MORPH_ERR_DB;
	*legacy = step == SQLITE_DONE;
	return 0;
}

static int import_error_value(nb_buf *sql, const char *value, bool legacy)
{
	if (legacy)
		return db_error_migration_append_status_sql(sql, value);
	return nb_buf_append_cstr(sql, value) ? 0 : nb_buf_status(sql);
}

static int import_session(sqlite3 *db, sqlite3_stmt *source, const char *origin,
			  struct runtime_session_import_result *result)
{
	int64_t old_id = sqlite3_column_int64(source, 0);
	const char *display = (const char *)sqlite3_column_text(source, 1);
	const char *name = (const char *)sqlite3_column_text(source, 2);
	const char *model = (const char *)sqlite3_column_text(source, 3);
	char revision[65];
	nb_buf identity NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
	sqlite3_stmt *stmt NB_CLEANUP(import_stmt_cleanup) = NULL;
	struct db target = { .handle = db };
	struct session session;
	int rc = import_revision(db, old_id, revision);

	if (rc != 0)
		return rc;
	(nb_buf_init(&identity), nb_buf_reserve(&identity, 128));
	(nb_buf_init(&title), nb_buf_reserve(&title, 128));
	nb_buf_printf(&identity, "%s:%s:%lld", origin, display ? display : "", (long long)old_id);
	if (import_has_column(db, "incoming", "sync_session_origins", "origin")) {
		if (sqlite3_prepare_v2(db,
			"SELECT origin FROM incoming.sync_session_origins WHERE session_id=?",
			-1, &stmt, NULL) != SQLITE_OK) {
			rc = (MORPH_ERR_DB);
			return rc;
		}
		sqlite3_bind_int64(stmt, 1, old_id);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			nb_buf_cleanup(&identity);
			(nb_buf_init(&identity), nb_buf_reserve(&identity, 128));
			nb_buf_append_cstr(&identity, (const char *)sqlite3_column_text(stmt, 0));
		}
		import_stmt_cleanup(&stmt);
	}
	if (nb_buf_status(&identity)) {
		rc = (nb_buf_status(&identity));
		return rc;
	}
	if (sqlite3_prepare_v2(db,
		"SELECT session_id FROM sync_import_receipts WHERE origin=? AND revision=?",
		-1, &stmt, NULL) != SQLITE_OK) {
		rc = (MORPH_ERR_DB);
		return rc;
	}
	sqlite3_bind_text(stmt, 1, nb_buf_cstr(&identity), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, revision, -1, SQLITE_TRANSIENT);
	int step = sqlite3_step(stmt);
	int64_t previous_id = step == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
	import_stmt_cleanup(&stmt);
	if (step == SQLITE_ROW) {
		if (sqlite3_prepare_v2(db,
			"INSERT INTO import_session_map(old_id,new_id,is_new) "
			"SELECT ?,id,0 FROM sessions WHERE id=?", -1, &stmt, NULL) != SQLITE_OK) {
			rc = (MORPH_ERR_DB);
			return rc;
		}
		sqlite3_bind_int64(stmt, 1, old_id);
		sqlite3_bind_int64(stmt, 2, previous_id);
		if (sqlite3_step(stmt) != SQLITE_DONE) {
			rc = (MORPH_ERR_DB);
			return rc;
		}
		result->unchanged++;
		return rc;
	}
	if (step != SQLITE_DONE) {
		rc = (MORPH_ERR_DB);
		return rc;
	}
	char *base = nb_utf8_ellipsize_bytes(name ? name : "Imported conversation", 180);
	if (!base) {
		rc = (NB_ERR_NOMEM);
		return rc;
	}
	nb_buf_append_cstr(&title, base);
	for (int suffix = 1; session_get_by_name(&target, nb_buf_cstr(&title), &session) == 0;
	     suffix++) {
		nb_buf_cleanup(&title);
		(nb_buf_init(&title), nb_buf_reserve(&title, 128));
		nb_buf_printf(&title, "%s [sync %d]", base, suffix);
	}
	free(base);
	if (nb_buf_status(&title)) {
		rc = (nb_buf_status(&title));
		return rc;
	}
	rc = session_create(&target, nb_buf_cstr(&title), model, &session);
	if (rc != 0)
		return rc;
	if (sqlite3_prepare_v2(db,
		"INSERT INTO import_session_map(old_id,new_id) VALUES(?,?)",
		-1, &stmt, NULL) != SQLITE_OK) {
		rc = (MORPH_ERR_DB);
		return rc;
	}
	sqlite3_bind_int64(stmt, 1, old_id);
	sqlite3_bind_int64(stmt, 2, session.id);
	if (sqlite3_step(stmt) != SQLITE_DONE)
		rc = (MORPH_ERR_DB);
	import_stmt_cleanup(&stmt);
	if (rc != 0)
		return rc;
	if (sqlite3_prepare_v2(db,
		"INSERT INTO sync_import_receipts(origin,revision,session_id) VALUES(?,?,?)",
		-1, &stmt, NULL) != SQLITE_OK) {
		rc = (MORPH_ERR_DB);
		return rc;
	}
	sqlite3_bind_text(stmt, 1, nb_buf_cstr(&identity), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, revision, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, session.id);
	if (sqlite3_step(stmt) != SQLITE_DONE)
		rc = (MORPH_ERR_DB);
	import_stmt_cleanup(&stmt);
	if (rc == 0 && sqlite3_prepare_v2(db,
		"INSERT INTO sync_session_origins(session_id,origin) VALUES(?,?)",
		-1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, session.id);
		sqlite3_bind_text(stmt, 2, nb_buf_cstr(&identity), -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) != SQLITE_DONE)
			rc = (MORPH_ERR_DB);
	} else {
		rc = (MORPH_ERR_DB);
	}
	if (rc == 0)
		result->imported++;

	return rc;
}

static int64_t import_offset(sqlite3 *db, const char *table)
{
	nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};
	sqlite3_stmt *stmt = NULL;
	int64_t value = -1;
	if (!import_has_column(db, "main", table, "id"))
		return 0;
	(nb_buf_init(&sql), nb_buf_reserve(&sql, 128));
	nb_buf_printf(&sql, "SELECT COALESCE(MAX(id),0) FROM main.%s", table);
	if (!nb_buf_status(&sql) &&
	    sqlite3_prepare_v2(db, nb_buf_cstr(&sql), -1, &stmt, NULL) == SQLITE_OK &&
	    sqlite3_step(stmt) == SQLITE_ROW)
		value = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	nb_buf_cleanup(&sql);
	return value;
}

static int import_table(sqlite3 *db, const char *table, int64_t message_offset,
			int64_t model_offset, bool legacy_errors)
{
	nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf columns NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf values NB_CLEANUP(nb_buf_cleanup) = {0};
	sqlite3_stmt *stmt NB_CLEANUP(import_stmt_cleanup) = NULL;
	int64_t offset = import_offset(db, table);
	int rc = 0;
	int count = 0;
	int step;
	bool migrate_error = legacy_errors && !strcmp(table, "history_compaction_attempts");

	if (!strcmp(table, "agent_threads") || !strcmp(table, "agent_mailbox") ||
	    !strcmp(table, "agent_events"))
		return 0;
	if (!import_has_column(db, "incoming", table, "session_id"))
		return 0;
	if (offset < 0)
		return MORPH_ERR_DB;
	(nb_buf_init(&sql), nb_buf_reserve(&sql, 128));
	(nb_buf_init(&columns), nb_buf_reserve(&columns, 128));
	(nb_buf_init(&values), nb_buf_reserve(&values, 128));
	nb_buf_printf(&sql, "PRAGMA main.table_info(%s)", table);
	if (nb_buf_status(&sql) ||
	    sqlite3_prepare_v2(db, nb_buf_cstr(&sql), -1, &stmt, NULL) != SQLITE_OK) {
		rc = (MORPH_ERR_DB);
		return rc;
	}
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *name = (const char *)sqlite3_column_text(stmt, 1);
		if (!import_has_column(db, "incoming", table, name) &&
		    !(migrate_error && !strcmp(name, "legacy_error_code")))
			continue;
		if (count++) {
			nb_buf_append_char(&columns, ',');
			nb_buf_append_char(&values, ',');
		}
		nb_buf_printf(&columns, "\"%s\"", name);
		if (strcmp(name, "session_id") == 0)
			nb_buf_append_cstr(&values, "m.new_id");
		else if (strcmp(name, "id") == 0 || strcmp(name, "superseded_by") == 0)
			nb_buf_printf(&values, "s.\"%s\"+%lld", name, (long long)offset);
		else if (strcmp(name, "core_message_id") == 0)
			nb_buf_printf(&values, "s.core_message_id+%lld", (long long)message_offset);
		else if (strcmp(name, "summary_item_id") == 0)
			nb_buf_printf(&values, "s.summary_item_id+%lld", (long long)model_offset);
		else if (migrate_error && !strcmp(name, "error_code"))
			(void)db_error_migration_append_status_sql(&values, "s.error_code");
		else if (migrate_error && !strcmp(name, "legacy_error_code"))
			(void)nb_buf_append_cstr(&values, "s.error_code");
		else
			nb_buf_printf(&values, "s.\"%s\"", name);
	}
	if (step != SQLITE_DONE || count == 0 || nb_buf_status(&columns) ||
	    nb_buf_status(&values)) {
		rc = (MORPH_ERR_DB);
		return rc;
	}
	nb_buf_cleanup(&sql);
	(nb_buf_init(&sql), nb_buf_reserve(&sql, 128));
	nb_buf_printf(&sql,
		      "INSERT INTO main.%s(%s) SELECT %s FROM incoming.%s s "
		      "JOIN import_session_map m ON s.session_id=m.old_id WHERE m.is_new=1",
		      table, nb_buf_cstr(&columns), nb_buf_cstr(&values), table);
	rc = nb_buf_status(&sql) ? nb_buf_status(&sql) : import_exec(db, nb_buf_cstr(&sql));

	return rc;
}

static int import_subagent_tasks(sqlite3 *db, bool legacy_errors)
{
	nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};

	(void)nb_buf_append_cstr(
		&sql,
		"CREATE TEMP TABLE import_task_map AS "
		"SELECT t.task_id AS old_id,CASE WHEN EXISTS(SELECT 1 FROM "
		"main.sub_agent_tasks x WHERE x.task_id=t.task_id) "
		"THEN t.task_id||'@'||p.new_id ELSE t.task_id END AS new_id "
		"FROM incoming.sub_agent_tasks t JOIN import_session_map p ON "
		"p.old_id=t.parent_session_id WHERE p.is_new=1;"
		"INSERT INTO sub_agent_tasks(task_id,parent_session_id,child_session_id,agent_name,"
		"description,mode,status,result,error_code,legacy_error_code,iterations,started_at,"
		"ended_at) "
		"SELECT "
		"m.new_id,p.new_id,c.new_id,t.agent_name,t.description,t.mode,t.status,t.result,");
	int rc = import_error_value(&sql, "t.error_code", legacy_errors);
	if (rc != 0)
		return rc;
	(void)nb_buf_printf(
		&sql,
		",%s,t.iterations,t.started_at,t.ended_at "
		"FROM incoming.sub_agent_tasks t JOIN import_task_map m ON t.task_id=m.old_id "
		"JOIN import_session_map p ON p.old_id=t.parent_session_id "
		"LEFT JOIN import_session_map c ON c.old_id=t.child_session_id;"
		"INSERT INTO sub_agent_events(task_id,event_json,created_at) "
		"SELECT m.new_id,e.event_json,e.created_at FROM incoming.sub_agent_events e "
		"JOIN import_task_map m ON e.task_id=m.old_id;",
		legacy_errors ? "t.error_code"
		: import_has_column(db, "incoming", "sub_agent_tasks", "legacy_error_code")
			? "t.legacy_error_code"
			: "NULL");
	return nb_buf_ok(&sql) ? import_exec(db, nb_buf_cstr(&sql)) : nb_buf_status(&sql);
}

static int import_agent_threads(sqlite3 *db, bool legacy_errors)
{
	nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};

	(void)nb_buf_append_cstr(
		&sql,
		"INSERT INTO agent_threads(root_session_id,agent_id,parent_agent_id,session_id,"
		"model,effort,depth,description,status,result,error_code,legacy_error_code,"
		"iterations,started_at,ended_at) "
		"SELECT p.new_id,a.agent_id,a.parent_agent_id,c.new_id,a.model,a.effort,a.depth,"
		"a.description,CASE WHEN a.status IN (0,1) THEN 4 ELSE a.status END,a.result,");
	int rc = import_error_value(&sql, "a.error_code", legacy_errors);
	if (rc != 0)
		return rc;
	(void)nb_buf_printf(
		&sql,
		",%s,a.iterations,a.started_at,a.ended_at "
		"FROM incoming.agent_threads a JOIN import_session_map p ON "
		"p.old_id=a.root_session_id "
		"JOIN import_session_map c ON c.old_id=a.session_id WHERE p.is_new=1;",
		legacy_errors ? "a.error_code"
		: import_has_column(db, "incoming", "agent_threads", "legacy_error_code")
			? "a.legacy_error_code"
			: "NULL");
	return nb_buf_ok(&sql) ? import_exec(db, nb_buf_cstr(&sql)) : nb_buf_status(&sql);
}

static int import_file_execute(struct runtime *runtime, const char *path,
	const char *origin, struct runtime_session_import_result *result,
	sqlite3 **database, int *transaction)
{
	sqlite3_stmt *stmt NB_CLEANUP(import_stmt_cleanup) = NULL;
	int rc;
	int step;
	bool legacy_errors;

	if (sqlite3_open_v2(sqlite3_db_filename(runtime->context.database.handle, "main"),
		database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
		rc = (MORPH_ERR_DB);
		return rc;
	}
	sqlite3 *db = *database;
	sqlite3_busy_timeout(db, 5000);
	(void)sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, NULL);
	if (sqlite3_prepare_v2(db, "ATTACH DATABASE ? AS incoming", -1, &stmt, NULL) != SQLITE_OK) {
		rc = (MORPH_ERR_DB);
		return rc;
	}
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	step = sqlite3_step(stmt);
	import_stmt_cleanup(&stmt);
	if (step != SQLITE_DONE || !import_has_column(db, "incoming", "sessions", "display_id") ||
	    !import_has_column(db, "incoming", "messages", "session_id")) {
		rc = (NB_ERR_FORMAT);
		return rc;
	}
	rc = import_check(db, "PRAGMA incoming.foreign_key_check");
	if (rc != 0)
		return rc;
	rc = import_check(db,
		"SELECT 1 FROM incoming.messages m LEFT JOIN incoming.sessions s "
		"ON s.id=m.session_id WHERE s.id IS NULL LIMIT 1");
	if (rc != 0)
		return rc;
	if (import_has_column(db, "incoming", "schema_migrations", "version")) {
		rc = import_check(db,
			"SELECT 1 WHERE (SELECT COALESCE(MAX(version),0) FROM "
			"incoming.schema_migrations) "
			"> (SELECT COALESCE(MAX(version),0) FROM main.schema_migrations)");
		if (rc != 0)
			return rc;
	}
	rc = import_exec(db, "PRAGMA foreign_keys=ON; BEGIN IMMEDIATE;");
	if (rc != 0)
		return rc;
	*transaction = 1;
	rc = import_legacy_error_schema(db, &legacy_errors);
	if (rc != 0)
		return rc;
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
		return rc;
	if (sqlite3_prepare_v2(db,
		"SELECT id,display_id,name,model FROM incoming.sessions ORDER BY id",
		-1, &stmt, NULL) != SQLITE_OK) {
		rc = (MORPH_ERR_DB);
		return rc;
	}
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		rc = import_session(db, stmt, origin, result);
		if (rc != 0)
			break;
	}
	import_stmt_cleanup(&stmt);
	if (rc == 0 && step != SQLITE_DONE)
		rc = (MORPH_ERR_DB);
	int64_t message_offset = import_offset(db, "messages");
	int64_t model_offset = import_offset(db, "model_history_items");
	if (message_offset < 0 || model_offset < 0)
		rc = (MORPH_ERR_DB);
	if (rc == 0)
		rc = import_exec(db, "UPDATE sessions SET (created_at,updated_at,token_used)="
			"(SELECT s.created_at,s.updated_at,s.token_used FROM incoming.sessions s "
			"JOIN import_session_map m ON m.old_id=s.id WHERE m.new_id=sessions.id) "
			"WHERE id IN (SELECT new_id FROM import_session_map WHERE is_new=1)");
	for (size_t i = 0; rc == 0 && i < IMPORT_TABLE_COUNT; i++)
		rc = import_table(db, history_tables[i], message_offset, model_offset,
				  legacy_errors);
	if (rc == 0 && import_has_column(db, "incoming", "message_attachments", "message_id")) {
		nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};
		(nb_buf_init(&sql), nb_buf_reserve(&sql, 128));
		nb_buf_printf(&sql,
			      "INSERT INTO message_attachments(message_id,kind,path,sha256) "
			      "SELECT a.message_id+%lld,a.kind,a.path,a.sha256 "
			      "FROM incoming.message_attachments a JOIN incoming.messages s ON "
			      "s.id=a.message_id "
			      "JOIN import_session_map m ON m.old_id=s.session_id WHERE m.is_new=1",
			      (long long)message_offset);
		rc = nb_buf_status(&sql) ? nb_buf_status(&sql) : import_exec(db, nb_buf_cstr(&sql));
		nb_buf_cleanup(&sql);
	}
	if (rc == 0 && import_has_column(db, "incoming", "sub_agent_tasks", "task_id"))
		rc = import_subagent_tasks(db, legacy_errors);
	if (rc == 0 && import_has_column(db, "incoming", "agent_threads", "root_session_id"))
		rc = import_agent_threads(db, legacy_errors);
	if (rc == 0 && import_has_column(db, "incoming", "agent_threads", "assignment_json"))
		rc = import_exec(db,
			"UPDATE agent_threads AS target SET assignment_json=("
			"SELECT a.assignment_json FROM incoming.agent_threads a "
			"JOIN import_session_map p ON p.old_id=a.root_session_id "
			"WHERE p.is_new=1 AND p.new_id=target.root_session_id "
			"AND a.agent_id=target.agent_id) "
			"WHERE root_session_id IN (SELECT new_id FROM import_session_map WHERE is_new=1);");
	if (rc == 0 && import_has_column(db, "incoming", "agent_mailbox", "root_session_id"))
		rc = import_exec(db,
			"INSERT INTO agent_mailbox(root_session_id,sender,recipient,kind,body,consumed) "
			"SELECT p.new_id,a.sender,a.recipient,a.kind,a.body,a.consumed "
			"FROM incoming.agent_mailbox a JOIN import_session_map p "
			"ON p.old_id=a.root_session_id WHERE p.is_new=1;");
	if (rc == 0 && import_has_column(db, "incoming", "agent_events", "root_session_id"))
		rc = import_exec(db,
			"INSERT INTO agent_events(root_session_id,agent_id,event_json) "
			"SELECT p.new_id,a.agent_id,a.event_json FROM incoming.agent_events a "
			"JOIN import_session_map p ON p.old_id=a.root_session_id WHERE p.is_new=1;");
	if (rc == 0 && import_has_column(db, "incoming", "agent_budget_state", "root_session_id"))
		rc = import_exec(db,
			"INSERT INTO agent_budget_state(root_session_id,used,input_tokens,cached_tokens,"
			"output_tokens,reminder_level) SELECT p.new_id,a.used,a.input_tokens,"
			"a.cached_tokens,a.output_tokens,a.reminder_level FROM incoming.agent_budget_state a "
			"JOIN import_session_map p ON p.old_id=a.root_session_id WHERE 1 "
			"ON CONFLICT(root_session_id) DO UPDATE SET used=MAX(used,excluded.used),"
			"input_tokens=MAX(input_tokens,excluded.input_tokens),"
			"cached_tokens=MAX(cached_tokens,excluded.cached_tokens),"
			"output_tokens=MAX(output_tokens,excluded.output_tokens),"
			"reminder_level=MAX(reminder_level,excluded.reminder_level);");
	if (rc == 0)
		rc = import_check(db, "PRAGMA main.foreign_key_check");
	if (rc == 0)
		rc = import_exec(db, "COMMIT");
	if (rc == 0)
		*transaction = 0;

	return rc;
}

int runtime_session_import_file(struct runtime *runtime, const char *path,
	const char *origin, struct runtime_session_import_result *result)
{
	sqlite3 *db = NULL;
	int rc;
	int transaction = 0;

	if (!runtime || !path || !origin || !result)
		return NB_ERR_INVAL;
	memset(result, 0, sizeof(*result));
	if (!nb_file_exists(path))
		return NB_ERR_NOT_FOUND;
	rc = nb_mutex_trylock(runtime->context.execution_lock);
	if (rc != 0)
		return rc;
	rc = import_file_execute(runtime, path, origin, result, &db, &transaction);

	if (transaction)
		(void)import_exec(db, "ROLLBACK");
	if (db)
		sqlite3_close(db);
	nb_mutex_unlock(runtime->context.execution_lock);
	if (rc != 0)
		memset(result, 0, sizeof(*result));
	return rc;
}

int runtime_sync_import_backup(struct runtime *runtime,
	const struct morph_sync_config *cfg, const char *snapshot_id,
	struct runtime_session_import_result *result)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_2 = NB_OK;
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_3 = NB_OK;

	struct morph_sync_backup *backups = NULL;
	char origin[MORPH_SYNC_DEVICE_ID_MAX] = { 0 };
	char directory[PATH_MAX];
	char path[PATH_MAX];
	char token[64];
	int count = 0;
	int rc;

	if (!runtime || !cfg || !snapshot_id || !result)
		return NB_ERR_INVAL;
	memset(result, 0, sizeof(*result));
	rc = morph_sync_backups(cfg, "data.db", &backups, &count);
	if (rc != 0)
		return rc;
	for (int i = 0; i < count; i++) {
		if (strcmp(backups[i].snapshot_id, snapshot_id) == 0)
			(void)nb_str_copy_to(
				nb_str_from_parts(backups[i].device_id,
						  nb_utf8_clamp_bytes(backups[i].device_id,
								      sizeof(origin) - 1)),
				origin, (sizeof(origin) - 1) + 1);
	}
	morph_sync_backups_free(backups);
	if (!origin[0])
		return NB_ERR_NOT_FOUND;
	rc = ((file_status_1 =
		       nb_path_concat(&file_value_1, cfg->sync_dir, ".morph-sync/import")) != NB_OK
		      ? file_status_1
	      : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), directory,
						sizeof(directory))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_1);
	if (rc == 0)
		rc = nb_file_mkdir(directory, true, NULL);
	if (rc == 0)
		rc = ((file_status_2 = ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
					: !(nb_buf_clear(&file_value_2),
					    nb_buf_append_cstr(&file_value_2, "session_"))
						? NB_ERR_NOMEM
						: nb_random_hex(&file_value_2, 16, NULL))) != NB_OK
			      ? file_status_2
		      : (file_status_2 = nb_str_copy_to(nb_buf_str(&file_value_2), token,
							sizeof(token))) == NB_ERR_RANGE
			      ? NB_ERR_NO_SPACE
			      : file_status_2);
	if (rc == 0)
		rc = ((file_status_3 = nb_path_concat(&file_value_3, directory, token)) != NB_OK
			      ? file_status_3
		      : (file_status_3 = nb_str_copy_to(nb_buf_str(&file_value_3), path,
							sizeof(path))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_3);
	if (rc != 0)
		return rc;
	rc = morph_sync_restore_db(cfg, snapshot_id, path);
	if (rc == 0)
		rc = runtime_session_import_file(runtime, path, origin, result);
	(void)unlink(path);
	return rc;
}
