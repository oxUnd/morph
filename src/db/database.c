#include "database.h"
#include "util/log.h"
#include "util/error.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *schema_sql =
	"CREATE TABLE IF NOT EXISTS sessions ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"display_id TEXT UNIQUE,"
	"name TEXT UNIQUE NOT NULL,"
	"model TEXT,"
	"created_at INTEGER NOT NULL,"
	"updated_at INTEGER NOT NULL,"
	"token_used INTEGER DEFAULT 0);"

	"CREATE TABLE IF NOT EXISTS messages ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"role TEXT NOT NULL,"
	"content TEXT NOT NULL,"
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
	"CREATE INDEX IF NOT EXISTS idx_traces_session ON react_traces(session_id, round_no);"
	"CREATE INDEX IF NOT EXISTS idx_outputs_session ON outputs(session_id, created_at);"
	"CREATE INDEX IF NOT EXISTS idx_memory_facts_session_key "
	"ON memory_facts(session_id, key_name, is_current, updated_at);"
	"CREATE INDEX IF NOT EXISTS idx_memory_episodes_session "
	"ON memory_episodes(session_id, created_at);"
	"CREATE INDEX IF NOT EXISTS idx_memory_procedures_session "
	"ON memory_procedures(session_id, updated_at);";

int db_open(struct db *db, const char *path)
{
	if (!db || !path)
		return -EINVAL;
	memset(db, 0, sizeof(*db));
	strncpy(db->path, path, sizeof(db->path) - 1);
	int rc = sqlite3_open(path, &db->handle);
	if (rc != SQLITE_OK) {
		log_err("failed to open db %s: %s", path, sqlite3_errmsg(db->handle));
		sqlite3_close(db->handle);
		db->handle = NULL;
		MORPH_RETURN(MORPH_ERR_DB);
	}
	sqlite3_exec(db->handle, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
	sqlite3_exec(db->handle, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
	log_info("database opened: %s", path);
	return 0;
}

void db_close(struct db *db)
{
	if (db && db->handle) {
		sqlite3_close(db->handle);
		db->handle = NULL;
		log_info("database closed: %s", db->path);
	}
}

int db_exec(struct db *db, const char *sql)
{
	if (!db || !db->handle || !sql)
		return -EINVAL;
	char *err = NULL;
	int rc = sqlite3_exec(db->handle, sql, NULL, NULL, &err);
	if (rc != SQLITE_OK) {
		log_err("db exec error: %s", err);
		sqlite3_free(err);
		return -rc;
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

static int db_migrate_memory_columns(struct db *db)
{
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

int db_init_schema(struct db *db)
{
	if (!db || !db->handle)
		return -EINVAL;
	int rc = db_exec(db, schema_sql);
	if (rc != 0)
		return rc;
	rc = db_migrate_display_id(db);
	if (rc != 0)
		return rc;
	return db_migrate_memory_columns(db);
}