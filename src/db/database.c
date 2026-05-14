#include "database.h"
#include "util/log.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>

static const char *schema_sql =
	"CREATE TABLE IF NOT EXISTS sessions ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
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

	"CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id, created_at);"
	"CREATE INDEX IF NOT EXISTS idx_traces_session ON react_traces(session_id, round_no);"
	"CREATE INDEX IF NOT EXISTS idx_outputs_session ON outputs(session_id, created_at);";

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
		return -EIO;
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

int db_init_schema(struct db *db)
{
	if (!db || !db->handle)
		return -EINVAL;
	return db_exec(db, schema_sql);
}