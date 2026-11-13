#include "database.h"
#include "error_migration.h"
#include "nb/log.h"
#include "errors.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/cleanup.h"
#include "nb/utf8.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char schema_sql[] =
	"CREATE TABLE IF NOT EXISTS schema_migrations ("
	"version INTEGER PRIMARY KEY,"
	"name TEXT NOT NULL,"
	"applied_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS sessions ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"display_id TEXT UNIQUE,"
	"name TEXT UNIQUE NOT NULL,"
	"model TEXT,"
	"workdir TEXT NOT NULL DEFAULT '',"
	"created_at INTEGER NOT NULL,"
	"updated_at INTEGER NOT NULL,"
	"token_used INTEGER DEFAULT 0);"

	"CREATE TABLE IF NOT EXISTS session_skill_activations ("
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"skill_name TEXT NOT NULL,PRIMARY KEY(session_id,skill_name));"

	"CREATE TABLE IF NOT EXISTS messages ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"role TEXT NOT NULL,"
	"content TEXT NOT NULL,"
	"turn_id TEXT,"
	"token_count INTEGER NOT NULL,"
	"compressed INTEGER DEFAULT 0,"
	"created_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS message_attachments ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,"
	"kind TEXT NOT NULL,"
	"path TEXT NOT NULL,"
	"sha256 TEXT);"

	"CREATE TABLE IF NOT EXISTS react_traces ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"round_no INTEGER NOT NULL,"
	"steps_json TEXT NOT NULL,"
	"aborted INTEGER DEFAULT 0,"
	"created_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS exts ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"name TEXT UNIQUE NOT NULL,"
	"version TEXT,"
	"path TEXT NOT NULL,"
	"type TEXT NOT NULL,"
	"permissions INTEGER,"
	"enabled INTEGER DEFAULT 1,"
	"installed_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS outputs ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER REFERENCES sessions(id) ON DELETE SET NULL,"
	"kind TEXT NOT NULL,"
	"path TEXT NOT NULL,"
	"prompt TEXT,"
	"model TEXT,"
	"request_prompt TEXT,"
	"provider TEXT,"
	"tool_name TEXT,"
	"idempotency_key TEXT,"
	"tool_call_id TEXT,"
	"turn_id TEXT,"
	"recipe_json TEXT,"
	"mime TEXT,"
	"width INTEGER NOT NULL DEFAULT 0,"
	"height INTEGER NOT NULL DEFAULT 0,"
	"duration_seconds INTEGER NOT NULL DEFAULT 0,"
	"size_bytes INTEGER NOT NULL DEFAULT 0,"
	"created_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS memory_profiles ("
	"session_id INTEGER PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,"
	"profile_text TEXT NOT NULL DEFAULT '',"
	"updated_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS memory_facts ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"key_name TEXT NOT NULL,"
	"value_text TEXT NOT NULL,"
	"source_text TEXT,"
	"confidence REAL DEFAULT 1.0,"
	"category TEXT DEFAULT 'general',"
	"importance REAL DEFAULT 0.5,"
	"is_current INTEGER NOT NULL DEFAULT 1,"
	"valid_from INTEGER NOT NULL,"
	"valid_to INTEGER,"
	"superseded_by INTEGER,"
	"created_at INTEGER NOT NULL,"
	"updated_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS memory_episodes ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"task_type TEXT,"
	"summary_text TEXT NOT NULL,"
	"outcome_text TEXT,"
	"success INTEGER NOT NULL DEFAULT 1,"
	"entities TEXT,"
	"key_decisions TEXT,"
	"artifacts TEXT,"
	"tools_used TEXT,"
	"importance REAL DEFAULT 0.5,"
	"created_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS memory_procedures ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"rule_text TEXT NOT NULL,"
	"trigger_text TEXT,"
	"evidence_count INTEGER NOT NULL DEFAULT 1,"
	"updated_at INTEGER NOT NULL,"
	"UNIQUE(session_id, rule_text));"

	"CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id, created_at);"
	"CREATE INDEX IF NOT EXISTS idx_messages_session_order "
	"ON messages(session_id, created_at, id);"
	"CREATE INDEX IF NOT EXISTS idx_traces_session ON react_traces(session_id, round_no);"
	"CREATE INDEX IF NOT EXISTS idx_outputs_session ON outputs(session_id, created_at);"
	"CREATE INDEX IF NOT EXISTS idx_memory_facts_session_key "
	"ON memory_facts(session_id, key_name, is_current, updated_at);"
	"CREATE INDEX IF NOT EXISTS idx_memory_episodes_session "
	"ON memory_episodes(session_id, created_at);"
	"CREATE INDEX IF NOT EXISTS idx_memory_procedures_session "
	"ON memory_procedures(session_id, updated_at);";

static const char preference_schema_sql[] =
	"CREATE TABLE IF NOT EXISTS memory_scopes ("
	"session_id INTEGER PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,"
	"owner TEXT NOT NULL,project TEXT NOT NULL DEFAULT '',"
	"migrated INTEGER NOT NULL DEFAULT 0,generation INTEGER NOT NULL DEFAULT 0);"
	"CREATE INDEX IF NOT EXISTS idx_memory_scopes_owner_project "
	"ON memory_scopes(owner,project,session_id);"
	"CREATE TABLE IF NOT EXISTS memory_preferences ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,owner TEXT NOT NULL,"
	"scope TEXT NOT NULL CHECK(scope IN ('personal','project','session')),"
	"target TEXT NOT NULL,key TEXT NOT NULL,value TEXT,source TEXT NOT NULL,"
	"origin TEXT NOT NULL DEFAULT 'explicit',source_order INTEGER NOT NULL DEFAULT 0,"
	"session_id INTEGER NOT NULL,event_token TEXT NOT NULL,"
	"created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER)),"
	"UNIQUE(owner,scope,target,key,session_id,event_token));"
	"CREATE INDEX IF NOT EXISTS idx_preferences_resolve "
	"ON memory_preferences(owner,scope,target,key,id DESC);"
	"CREATE TABLE IF NOT EXISTS memory_candidates ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"kind TEXT NOT NULL,key TEXT NOT NULL,value TEXT NOT NULL,"
	"source TEXT NOT NULL,created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS "
	"INTEGER)),"
	"UNIQUE(session_id,kind,key,value,source));"
	"CREATE INDEX IF NOT EXISTS idx_memory_candidates_lookup "
	"ON memory_candidates(session_id,kind,key,created_at DESC,id DESC);"
	"CREATE TABLE IF NOT EXISTS memory_jobs ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"generation INTEGER NOT NULL,payload TEXT NOT NULL,"
	"state TEXT NOT NULL DEFAULT 'queued',attempts INTEGER NOT NULL DEFAULT 0,"
	"worker_pid INTEGER NOT NULL DEFAULT 0,error_code INTEGER NOT NULL DEFAULT 0,"
	"updated_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER)));";

static const char sub_agent_schema_sql[] =
	"CREATE TABLE IF NOT EXISTS sub_agent_tasks ("
	"task_id TEXT PRIMARY KEY,"
	"parent_session_id INTEGER REFERENCES sessions(id) ON DELETE CASCADE,"
	"child_session_id INTEGER REFERENCES sessions(id) ON DELETE CASCADE,"
	"agent_name TEXT NOT NULL,"
	"description TEXT NOT NULL,"
	"mode TEXT NOT NULL,"
	"status INTEGER NOT NULL,"
	"result TEXT,"
	"error_code INTEGER NOT NULL DEFAULT 0,"
	"iterations INTEGER NOT NULL DEFAULT 0,"
	"started_at INTEGER NOT NULL,"
	"ended_at INTEGER NOT NULL DEFAULT 0);"
	"CREATE TABLE IF NOT EXISTS sub_agent_events ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"task_id TEXT NOT NULL REFERENCES sub_agent_tasks(task_id) "
	"ON DELETE CASCADE,"
	"event_json TEXT NOT NULL,"
	"created_at INTEGER NOT NULL);"
	"CREATE INDEX IF NOT EXISTS idx_sub_agent_tasks_parent "
	"ON sub_agent_tasks(parent_session_id,started_at);"
	"CREATE INDEX IF NOT EXISTS idx_sub_agent_events_task "
	"ON sub_agent_events(task_id,id);";

static const char agent_schema_sql[] =
	"CREATE TABLE IF NOT EXISTS agent_threads ("
	"root_session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"agent_id TEXT NOT NULL,parent_agent_id TEXT NOT NULL,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"model TEXT NOT NULL,effort TEXT NOT NULL,depth INTEGER NOT NULL,"
	"description TEXT NOT NULL,assignment_json TEXT,status INTEGER NOT NULL,result TEXT,"
	"error_code INTEGER NOT NULL DEFAULT 0,iterations INTEGER NOT NULL DEFAULT 0,"
	"started_at INTEGER NOT NULL,ended_at INTEGER NOT NULL DEFAULT 0,"
	"PRIMARY KEY(root_session_id,agent_id));"
	"CREATE TABLE IF NOT EXISTS agent_mailbox ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"root_session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"sender TEXT NOT NULL,recipient TEXT NOT NULL,kind TEXT NOT NULL,"
	"body TEXT NOT NULL,consumed INTEGER NOT NULL DEFAULT 0);"
	"CREATE INDEX IF NOT EXISTS idx_agent_mailbox_recipient ON "
	"agent_mailbox(root_session_id,recipient,consumed,id);"
	"CREATE TABLE IF NOT EXISTS agent_events ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,root_session_id INTEGER NOT NULL "
	"REFERENCES sessions(id) ON DELETE CASCADE,agent_id TEXT NOT NULL,"
	"event_json TEXT NOT NULL);"
	"CREATE TABLE IF NOT EXISTS agent_budget_state ("
	"root_session_id INTEGER PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,"
	"used INTEGER NOT NULL DEFAULT 0,input_tokens INTEGER NOT NULL DEFAULT 0,"
	"cached_tokens INTEGER NOT NULL DEFAULT 0,output_tokens INTEGER NOT NULL DEFAULT 0,"
	"reminder_level INTEGER NOT NULL DEFAULT 0);";

static const char history_schema_sql[] =
	"CREATE TABLE IF NOT EXISTS model_history_items ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"sequence_no INTEGER NOT NULL,"
	"turn_id TEXT,"
	"kind TEXT NOT NULL,"
	"role TEXT,"
	"content TEXT,"
	"payload_json TEXT,"
	"tool_call_id TEXT,"
	"provider_call_id TEXT,"
	"tool_name TEXT,"
	"idempotency_key TEXT,"
	"token_count INTEGER NOT NULL DEFAULT 0,"
	"truncated INTEGER NOT NULL DEFAULT 0,"
	"active INTEGER NOT NULL DEFAULT 1,"
	"created_at INTEGER NOT NULL,"
	"UNIQUE(session_id,sequence_no));"
	"CREATE TABLE IF NOT EXISTS history_compactions ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"turn_id TEXT,"
	"cutoff_sequence_no INTEGER NOT NULL,"
	"summary_item_id INTEGER NOT NULL REFERENCES model_history_items(id),"
	"input_tokens INTEGER NOT NULL,"
	"output_tokens INTEGER NOT NULL,"
	"trigger_kind TEXT NOT NULL,"
	"created_at INTEGER NOT NULL);"
	"CREATE INDEX IF NOT EXISTS idx_model_history_active "
	"ON model_history_items(session_id,active,sequence_no);"
	"CREATE TABLE IF NOT EXISTS history_summary_sources ("
	"summary_item_id INTEGER NOT NULL REFERENCES model_history_items(id) ON DELETE CASCADE,"
	"source_item_id INTEGER NOT NULL REFERENCES model_history_items(id) ON DELETE CASCADE,"
	"PRIMARY KEY(summary_item_id,source_item_id));"
	"CREATE TABLE IF NOT EXISTS model_request_snapshots ("
	"session_id INTEGER PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,"
	"turn_id TEXT,provider TEXT NOT NULL,model TEXT NOT NULL,"
	"input_json TEXT NOT NULL,created_at INTEGER NOT NULL);"
	"CREATE INDEX IF NOT EXISTS idx_model_history_turn "
	"ON model_history_items(session_id,turn_id,sequence_no);"
	"CREATE INDEX IF NOT EXISTS idx_model_history_call "
	"ON model_history_items(session_id,tool_call_id);"
	"CREATE TABLE IF NOT EXISTS history_compaction_attempts ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"turn_id TEXT,"
	"trigger_kind TEXT NOT NULL,"
	"status TEXT NOT NULL,"
	"input_tokens INTEGER NOT NULL DEFAULT 0,"
	"output_tokens INTEGER NOT NULL DEFAULT 0,"
	"error_code INTEGER NOT NULL DEFAULT 0,"
	"error_text TEXT,"
	"created_at INTEGER NOT NULL);"
	"CREATE INDEX IF NOT EXISTS idx_history_compaction_attempts_session "
	"ON history_compaction_attempts(session_id,created_at,id);";

static const char permission_schema_sql[] =
    "CREATE TABLE IF NOT EXISTS session_plans ("
    "session_id INTEGER PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,"
    "state_json TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS permission_grants ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "subject TEXT NOT NULL,"
    "resource_kind TEXT NOT NULL,"
    "resource TEXT NOT NULL,"
    "project_root TEXT NOT NULL,"
    "created_at INTEGER NOT NULL,"
    "UNIQUE(subject,resource_kind,resource,project_root));"
    "CREATE INDEX IF NOT EXISTS idx_permission_grants_project "
    "ON permission_grants(project_root,subject,resource_kind);";

static const char schema_baseline_sql[] =
	"INSERT OR IGNORE INTO schema_migrations(version,name,applied_at) "
	"VALUES(1,'baseline',strftime('%s','now'));";

static const char scheduled_schema_sql[] =
	"CREATE TABLE IF NOT EXISTS scheduled_tasks ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"source_session_id INTEGER REFERENCES sessions(id) ON DELETE SET NULL,"
	"latest_session_id INTEGER REFERENCES sessions(id) ON DELETE SET NULL,"
	"title TEXT NOT NULL,"
	"kind TEXT NOT NULL,"
	"status TEXT NOT NULL,"
	"trigger_type TEXT NOT NULL,"
	"next_run_at INTEGER NOT NULL,"
	"interval_seconds INTEGER NOT NULL DEFAULT 0,"
	"timeout_at INTEGER NOT NULL DEFAULT 0,"
	"attempts INTEGER NOT NULL DEFAULT 0,"
	"max_attempts INTEGER NOT NULL DEFAULT 0,"
	"action_type TEXT NOT NULL,"
	"payload_json TEXT,"
	"policy_json TEXT,"
	"notify_json TEXT,"
	"last_error TEXT,"
	"created_at INTEGER NOT NULL,"
	"updated_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS notifications ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"task_id INTEGER REFERENCES scheduled_tasks(id) ON DELETE SET NULL,"
	"session_id INTEGER REFERENCES sessions(id) ON DELETE SET NULL,"
	"level TEXT NOT NULL,"
	"title TEXT NOT NULL,"
	"body TEXT NOT NULL,"
	"created_at INTEGER NOT NULL,"
	"read_at INTEGER NOT NULL DEFAULT 0,"
	"delivery_status TEXT NOT NULL);"

	"CREATE INDEX IF NOT EXISTS idx_scheduled_tasks_due "
	"ON scheduled_tasks(status, next_run_at);"
	"CREATE INDEX IF NOT EXISTS idx_notifications_unread "
	"ON notifications(read_at, created_at);";

static const char credit_schema_sql[] =
	"CREATE TABLE IF NOT EXISTS credit_events ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"user_id TEXT,"
	"session_id TEXT,"
	"kind TEXT NOT NULL,"
	"provider TEXT,"
	"model TEXT,"
	"input_tokens INTEGER NOT NULL DEFAULT 0,"
	"output_tokens INTEGER NOT NULL DEFAULT 0,"
	"image_units INTEGER NOT NULL DEFAULT 0,"
	"video_seconds INTEGER NOT NULL DEFAULT 0,"
	"estimated_cost REAL NOT NULL DEFAULT 0,"
	"currency TEXT NOT NULL DEFAULT 'USD',"
	"credits INTEGER NOT NULL DEFAULT 0,"
	"metadata_json TEXT,"
	"created_at INTEGER NOT NULL);"
	"CREATE INDEX IF NOT EXISTS idx_credit_events_user_day "
	"ON credit_events(user_id, created_at);"
	"CREATE INDEX IF NOT EXISTS idx_credit_events_session "
	"ON credit_events(session_id, created_at);";

static void db_text_contains(sqlite3_context *context, int count, sqlite3_value **values)
{
	(void)count;
	const char *text = (const char *)sqlite3_value_text(values[0]);
	const char *query = (const char *)sqlite3_value_text(values[1]);
	nb_buf folded_text NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf folded_query NB_CLEANUP(nb_buf_destroy) = {0};

	if (!text || !query) {
		sqlite3_result_int(context, 0);
		return;
	}
	if (nb_utf8_validate(nb_str_from_cstr(text), NULL) ||
	    nb_utf8_validate(nb_str_from_cstr(query), NULL)) {
		sqlite3_result_error(context, "Invalid UTF-8 text", -1);
		return;
	}
	if (nb_utf8_casefold(nb_str_from_cstr(text), &folded_text, NULL) != NB_OK ||
	    nb_utf8_casefold(nb_str_from_cstr(query), &folded_query, NULL) != NB_OK) {
		sqlite3_result_error_nomem(context);
		return;
	}
	sqlite3_result_int(context,
			   strstr(nb_buf_cstr(&folded_text), nb_buf_cstr(&folded_query)) != NULL);
}

int db_open(struct nb_log *logger, struct db *db, const char *path)
{
	if (!db || !path)
		return NB_ERR_INVAL;
	memset(db, 0, sizeof(*db));
	db->logger = logger;
	strncpy(db->path, path, sizeof(db->path) - 1);
	int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
		SQLITE_OPEN_FULLMUTEX;
	int rc = sqlite3_open_v2(path, &db->handle, flags, NULL);
	if (rc != SQLITE_OK) {
		nb_log_write(logger, NB_LOG_ERROR, "failed to open db %s: %s", path,
			     sqlite3_errmsg(db->handle));
		sqlite3_close(db->handle);
		db->handle = NULL;
		return nb_err_trace(logger, MORPH_ERR_DB, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	}
	rc = sqlite3_create_function_v2(db->handle, "morph_text_contains", 2,
					SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, db_text_contains,
					NULL, NULL, NULL);
	if (rc != SQLITE_OK) {
		sqlite3_close(db->handle);
		db->handle = NULL;
		return MORPH_ERR_DB;
	}
	sqlite3_exec(db->handle, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
	sqlite3_exec(db->handle, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
	/* Multiple frontends and background workers may write this WAL database. */
	sqlite3_busy_timeout(db->handle, 5000);
	nb_log_write(logger, NB_LOG_INFO, "database opened: %s", path);
	return 0;
}

int db_lock_acquire(struct db *db, struct db_lock *lock)
{
	if (!db || !db->handle || !lock)
		return NB_ERR_INVAL;
	if (lock->mutex)
		return NB_ERR_EXISTS;
	sqlite3_mutex *mutex = sqlite3_db_mutex(db->handle);

	if (!mutex)
		return NB_ERR_UNSUPPORTED;
	sqlite3_mutex_enter(mutex);
	lock->mutex = mutex;
	return 0;
}

void db_lock_cleanup(struct db_lock *lock)
{
	if (lock && lock->mutex) {
		sqlite3_mutex_leave(lock->mutex);
		lock->mutex = NULL;
	}
}

void db_close(struct db *db)
{
	if (db && db->handle) {
		sqlite3_close(db->handle);
		db->handle = NULL;
		nb_log_write(db ? db->logger : NULL, NB_LOG_INFO, "database closed: %s", db->path);
	}
}

int db_exec(struct db *db, const char *sql)
{
	if (!db || !db->handle || !sql)
		return NB_ERR_INVAL;
	char *err = NULL;
	int rc = sqlite3_exec(db->handle, sql, NULL, NULL, &err);
	if (rc != SQLITE_OK) {
		nb_log_write(db ? db->logger : NULL, NB_LOG_ERROR, "db exec error: %s", err);
		sqlite3_free(err);
		return nb_err_trace(db->logger, MORPH_ERR_DB, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	}
	return 0;
}

static int db_migrate_display_id(struct db *db)
{
	sqlite3_stmt *stmt;
	const char *sql = "PRAGMA table_info(sessions)";
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		return 0;
	int has = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *cname = (const char *)sqlite3_column_text(stmt, 1);
		if (cname && strcmp(cname, "display_id") == 0) {
			has = 1;
			break;
		}
	}
	sqlite3_finalize(stmt);
	if (!has)
		db_exec(db, "ALTER TABLE sessions ADD COLUMN display_id TEXT UNIQUE");
	return 0;
}

static int db_table_has_column(struct db *db, const char *table,
			       const char *column)
{
	sqlite3_stmt *stmt = NULL;
	char sql[128];
	int has = 0;

	snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		return 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *cname = (const char *)sqlite3_column_text(stmt, 1);
		if (cname && strcmp(cname, column) == 0) {
			has = 1;
			break;
		}
	}
	sqlite3_finalize(stmt);
	return has;
}

static void db_add_column_if_missing(struct db *db, const char *table,
				     const char *column, const char *type_def)
{
	char sql[256];

	if (db_table_has_column(db, table, column))
		return;
	snprintf(sql, sizeof(sql),
		 "ALTER TABLE %s ADD COLUMN %s %s",
		 table, column, type_def);
	db_exec(db, sql);
}

static int db_migrate_session_workdir(struct db *db)
{
	db_add_column_if_missing(db, "sessions", "workdir", "TEXT NOT NULL DEFAULT ''");
	if (!db_table_has_column(db, "sessions", "workdir"))
		return nb_err_trace(db->logger, MORPH_ERR_DB, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	return 0;
}

static int db_migrate_memory_columns(struct db *db)
{
	db_add_column_if_missing(db, "memory_scopes", "content_revision",
				 "INTEGER NOT NULL DEFAULT 0");
	db_add_column_if_missing(db, "memory_jobs", "content_revision",
				 "INTEGER NOT NULL DEFAULT -1");
	db_add_column_if_missing(db, "memory_jobs", "stage", "TEXT NOT NULL DEFAULT 'extract'");
	db_add_column_if_missing(db, "memory_preferences", "origin",
		"TEXT NOT NULL DEFAULT 'explicit'");
	db_add_column_if_missing(db, "memory_preferences", "source_order",
		"INTEGER NOT NULL DEFAULT 0");
	db_add_column_if_missing(db, "memory_scopes", "generation",
		"INTEGER NOT NULL DEFAULT 0");
	if (!db_table_has_column(db, "memory_preferences", "origin") ||
	    !db_table_has_column(db, "memory_preferences", "source_order") ||
	    !db_table_has_column(db, "memory_scopes", "generation") ||
	    !db_table_has_column(db, "memory_scopes", "content_revision") ||
	    !db_table_has_column(db, "memory_jobs", "stage") ||
	    !db_table_has_column(db, "memory_jobs", "content_revision"))
		return nb_err_trace(db->logger, MORPH_ERR_DB, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	db_add_column_if_missing(db, "memory_episodes", "job_id", "INTEGER");
	if (!db_table_has_column(db, "memory_episodes", "job_id"))
		return nb_err_trace(db->logger, MORPH_ERR_DB, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	if (db_exec(db, "CREATE UNIQUE INDEX IF NOT EXISTS idx_memory_episode_job "
		"ON memory_episodes(job_id) WHERE job_id IS NOT NULL") != 0)
		return nb_err_trace(db->logger, MORPH_ERR_DB, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	db_add_column_if_missing(db, "memory_facts", "category",
				 "TEXT DEFAULT 'general'");
	db_add_column_if_missing(db, "memory_facts", "importance",
				 "REAL DEFAULT 0.5");
	db_add_column_if_missing(db, "memory_episodes", "key_decisions",
				 "TEXT");
	db_add_column_if_missing(db, "memory_episodes", "artifacts", "TEXT");
	db_add_column_if_missing(db, "memory_episodes", "tools_used", "TEXT");
	db_add_column_if_missing(db, "memory_episodes", "importance",
				 "REAL DEFAULT 0.5");
	return 0;
}

static int db_migrate_message_columns(struct db *db)
{
	db_add_column_if_missing(db, "messages", "turn_id", "TEXT");
	return 0;
}

static int db_migrate_output_columns(struct db *db)
{
	db_add_column_if_missing(db, "outputs", "request_prompt", "TEXT");
	db_add_column_if_missing(db, "outputs", "provider", "TEXT");
	db_add_column_if_missing(db, "outputs", "tool_name", "TEXT");
	db_add_column_if_missing(db, "outputs", "tool_call_id", "TEXT");
	db_add_column_if_missing(db, "outputs", "turn_id", "TEXT");
	db_add_column_if_missing(db, "outputs", "recipe_json", "TEXT");
	db_add_column_if_missing(db, "outputs", "mime", "TEXT");
	db_add_column_if_missing(db, "outputs", "width",
				 "INTEGER NOT NULL DEFAULT 0");
	db_add_column_if_missing(db, "outputs", "height",
				 "INTEGER NOT NULL DEFAULT 0");
	db_add_column_if_missing(db, "outputs", "duration_seconds",
				 "INTEGER NOT NULL DEFAULT 0");
	db_add_column_if_missing(db, "outputs", "size_bytes",
				 "INTEGER NOT NULL DEFAULT 0");
	return db_exec(db, "CREATE INDEX IF NOT EXISTS idx_outputs_path "
			      "ON outputs(path, id DESC)");
}

static int db_migrate_scheduled_task_columns(struct db *db)
{
	db_add_column_if_missing(db, "scheduled_tasks", "run_token", "TEXT NOT NULL DEFAULT ''");
	int token_rc = db_exec(db, "SELECT run_token FROM scheduled_tasks LIMIT 0");
	if (token_rc != 0) {
		return token_rc;
	}
	db_add_column_if_missing(db, "scheduled_tasks", "source_session_id",
				 "INTEGER REFERENCES sessions(id) ON DELETE SET NULL");
	db_add_column_if_missing(db, "scheduled_tasks", "latest_session_id",
				 "INTEGER REFERENCES sessions(id) ON DELETE SET NULL");
	db_add_column_if_missing(db, "notifications", "session_id",
				 "INTEGER REFERENCES sessions(id) ON DELETE SET NULL");
	return 0;
}

static int db_retire_memory_receipts(struct db *db)
{
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db->handle,
		"SELECT 1 FROM schema_migrations WHERE version=2", -1, &stmt, NULL);

	if (rc != SQLITE_OK)
		return nb_err_trace(db->logger, MORPH_ERR_DB, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc == SQLITE_ROW)
		return 0;
	if (rc != SQLITE_DONE)
		return nb_err_trace(db->logger, MORPH_ERR_DB, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	rc = db_exec(db, "SAVEPOINT retire_memory_receipts;");
	if (rc != 0)
		return rc;
	rc = db_exec(db,
		"UPDATE model_history_items SET active=0 "
		"WHERE kind='background_receipt' AND active=1 AND "
		"substr(content,1,length('[Background receipt] memory_consolidation:'))="
		"'[Background receipt] memory_consolidation:';"
		"INSERT OR IGNORE INTO schema_migrations(version,name,applied_at) "
		"VALUES(2,'retire_memory_maintenance_receipts',strftime('%s','now'));"
		"RELEASE retire_memory_receipts;");
	if (rc != 0)
		(void)db_exec(db, "ROLLBACK TO retire_memory_receipts; RELEASE retire_memory_receipts;");
	return rc;
}

int db_init_credit_schema(struct db *db)
{
	if (!db || !db->handle)
		return NB_ERR_INVAL;
	return db_exec(db, credit_schema_sql);
}

/* Revisions cover both direct edits and background writes. A generated summary
 * is readable only while every source in its session snapshot is unchanged. */
static int db_init_memory_notes(struct db *db)
{
	static const char *const tables[] = {
		"memory_facts",	     "memory_candidates", "memory_episodes",
		"memory_procedures", "memory_notes",
	};
	static const char *const events[] = {"INSERT", "UPDATE", "DELETE"};
	int rc = db_exec(
		db, "CREATE TABLE IF NOT EXISTS memory_notes ("
		    "id INTEGER PRIMARY KEY AUTOINCREMENT,session_id INTEGER NOT NULL "
		    "REFERENCES sessions(id) ON DELETE CASCADE,title TEXT NOT NULL,"
		    "text TEXT NOT NULL,source TEXT NOT NULL,event_token TEXT NOT NULL,"
		    "scope TEXT NOT NULL DEFAULT 'project' CHECK(scope IN ('project','session')),"
		    "created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER)),"
		    "updated_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER)),"
		    "UNIQUE(session_id,event_token,title));"
		    "CREATE TABLE IF NOT EXISTS memory_summaries ("
		    "session_id INTEGER PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,"
		    "memory_version INTEGER NOT NULL,content_revision INTEGER NOT NULL,"
		    "title TEXT NOT NULL,text TEXT NOT NULL,sources TEXT NOT NULL,"
		    "updated_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER)));"
		    "CREATE INDEX IF NOT EXISTS idx_memory_notes_session ON "
		    "memory_notes(session_id,id);");

	for (size_t table = 0; rc == 0 && table < sizeof(tables) / sizeof(tables[0]); table++) {
		for (size_t event = 0; rc == 0 && event < sizeof(events) / sizeof(events[0]);
		     event++) {
			nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};
			const char *row = event == 2 ? "OLD" : "NEW";

			rc = (nb_buf_init(&sql), nb_buf_reserve(&sql, 512) ? 0 : NB_ERR_NOMEM);
			if (rc == 0)
				rc = (nb_buf_printf(&sql,
						    "CREATE TRIGGER IF NOT EXISTS "
						    "%s_revision_%zu AFTER %s ON %s "
						    "BEGIN UPDATE memory_scopes SET "
						    "content_revision=content_revision+1 "
						    "WHERE session_id=%s.session_id; END;",
						    tables[table], event, events[event],
						    tables[table], row)
					      ? 0
					      : NB_ERR_NOMEM);
			if (rc == 0)
				rc = db_exec(db, nb_buf_cstr(&sql));
		}
	}
	return rc;
}

int db_init_schema(struct db *db)
{
	if (!db || !db->handle)
		return NB_ERR_INVAL;
	int rc = db_exec(db, schema_sql);
	if (rc != 0)
		return rc;
	rc = db_exec(db, preference_schema_sql);
	if (rc != 0)
		return rc;
	rc = db_exec(db, agent_schema_sql);
	if (rc != 0)
		return rc;
	db_add_column_if_missing(db, "agent_threads", "assignment_json", "TEXT");
	if (!db_table_has_column(db, "agent_threads", "assignment_json"))
		return nb_err_trace(db->logger, MORPH_ERR_DB, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	rc = db_exec(db, sub_agent_schema_sql);
	if (rc != 0)
		return rc;
	rc = db_exec(db, history_schema_sql);
	if (rc != 0)
		return rc;
	rc = db_retire_memory_receipts(db);
	if (rc != 0)
		return rc;
	db_add_column_if_missing(db, "model_history_items",
		"idempotency_key", "TEXT");
	if (!db_table_has_column(db, "model_history_items", "idempotency_key"))
		return nb_err_trace(db->logger, MORPH_ERR_DB, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	rc = db_exec(db,
		"CREATE UNIQUE INDEX IF NOT EXISTS idx_model_history_idempotency "
		"ON model_history_items(session_id,idempotency_key) "
		"WHERE idempotency_key IS NOT NULL;");
	if (rc != 0)
		return rc;
	rc = db_exec(db, scheduled_schema_sql);
	if (rc != 0)
		return rc;
	rc = db_exec(db, permission_schema_sql);
	if (rc != 0)
		return rc;
	rc = db_migrate_display_id(db);
	if (rc != 0)
		return rc;
	rc = db_migrate_session_workdir(db);
	if (rc != 0)
		return rc;
	rc = db_migrate_scheduled_task_columns(db);
	if (rc != 0)
		return rc;
	rc = db_migrate_memory_columns(db);
	if (rc != 0)
		return rc;
	rc = db_init_memory_notes(db);
	if (rc != 0)
		return rc;
	rc = db_migrate_message_columns(db);
	if (rc != 0)
		return rc;
	rc = db_migrate_output_columns(db);
	if (rc != 0)
		return rc;
	rc = db_init_credit_schema(db);
	if (rc != 0)
		return rc;
	rc = db_migrate_error_codes(db);
	if (rc != 0)
		return rc;
	return db_exec(db, schema_baseline_sql);
}
