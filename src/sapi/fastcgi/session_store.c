#include "sapi/fastcgi/session_store.h"
#include "sapi/fastcgi/session_store_internal.h"
#include "sapi/fastcgi/storage/events.h"
#include "errors.h"

#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char SCHEMA[] =
	"PRAGMA journal_mode=WAL;"
	"PRAGMA synchronous=NORMAL;"
	"PRAGMA busy_timeout=5000;"
	"CREATE TABLE IF NOT EXISTS fcgi_events ("
	"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"  session_id TEXT NOT NULL,"
	"  type TEXT NOT NULL,"
	"  payload TEXT NOT NULL,"
	"  ts INTEGER NOT NULL"
	");"
	"CREATE INDEX IF NOT EXISTS idx_events_sess ON fcgi_events(session_id, id);"
	"CREATE TABLE IF NOT EXISTS fcgi_actions ("
	"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"  session_id TEXT NOT NULL,"
	"  type TEXT NOT NULL,"
	"  payload TEXT NOT NULL,"
	"  ts INTEGER NOT NULL,"
	"  consumed INTEGER NOT NULL DEFAULT 0"
	");"
	"CREATE INDEX IF NOT EXISTS idx_actions_sess "
	"  ON fcgi_actions(session_id, consumed, id);"
	"CREATE TABLE IF NOT EXISTS fcgi_canvas_nodes ("
	"  id TEXT PRIMARY KEY,"
	"  session_id TEXT NOT NULL,"
	"  parent_id TEXT,"
	"  kind TEXT NOT NULL,"
	"  x REAL, y REAL, w REAL, h REAL,"
	"  content_ref TEXT,"
	"  meta TEXT,"
	"  created_at INTEGER NOT NULL,"
	"  updated_at INTEGER NOT NULL"
	");"
	"CREATE INDEX IF NOT EXISTS idx_canvas_sess "
	"  ON fcgi_canvas_nodes(session_id, updated_at);"
	"CREATE TABLE IF NOT EXISTS fcgi_session_owner ("
	"  session_id TEXT PRIMARY KEY,"
	"  user_id TEXT NOT NULL"
	");"
	"CREATE TABLE IF NOT EXISTS fcgi_users ("
	"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"  user_id TEXT NOT NULL UNIQUE,"
	"  username TEXT NOT NULL UNIQUE,"
	"  password_hash TEXT NOT NULL,"
	"  role TEXT NOT NULL DEFAULT 'user',"
	"  disabled INTEGER NOT NULL DEFAULT 0,"
	"  created_at INTEGER NOT NULL,"
	"  updated_at INTEGER NOT NULL"
	");"
	"CREATE TABLE IF NOT EXISTS fcgi_quota_profiles ("
	"  name TEXT PRIMARY KEY,"
	"  daily_turns INTEGER NOT NULL DEFAULT 100,"
	"  daily_images INTEGER NOT NULL DEFAULT 20,"
	"  daily_videos INTEGER NOT NULL DEFAULT 5,"
	"  storage_bytes INTEGER NOT NULL DEFAULT 1073741824,"
	"  max_concurrent_turns INTEGER NOT NULL DEFAULT 1"
	");"
	"CREATE TABLE IF NOT EXISTS fcgi_user_quota ("
	"  user_id TEXT PRIMARY KEY,"
	"  profile TEXT NOT NULL DEFAULT 'free'"
	");"
	"CREATE TABLE IF NOT EXISTS fcgi_usage_daily ("
	"  user_id TEXT NOT NULL,"
	"  day TEXT NOT NULL,"
	"  turns INTEGER NOT NULL DEFAULT 0,"
	"  images INTEGER NOT NULL DEFAULT 0,"
	"  videos INTEGER NOT NULL DEFAULT 0,"
	"  PRIMARY KEY(user_id, day)"
	");"
	"CREATE TABLE IF NOT EXISTS fcgi_usage_storage ("
	"  user_id TEXT PRIMARY KEY,"
	"  used_bytes INTEGER NOT NULL DEFAULT 0"
	");"
	"CREATE TABLE IF NOT EXISTS fcgi_running_turns ("
	"  id TEXT PRIMARY KEY,"
	"  user_id TEXT NOT NULL,"
	"  session_id TEXT NOT NULL,"
	"  started_at INTEGER NOT NULL,"
	"  heartbeat_at INTEGER NOT NULL"
	");"
	"CREATE TABLE IF NOT EXISTS fcgi_artifacts ("
	"  id TEXT PRIMARY KEY,"
	"  user_id TEXT NOT NULL,"
	"  session_id TEXT NOT NULL,"
	"  kind TEXT NOT NULL,"
	"  mime TEXT NOT NULL,"
	"  filename TEXT NOT NULL,"
	"  relative_path TEXT NOT NULL,"
	"  size_bytes INTEGER NOT NULL DEFAULT 0,"
	"  status TEXT NOT NULL DEFAULT 'ready',"
	"  created_at INTEGER NOT NULL,"
	"  expires_at INTEGER"
	");"
	"CREATE INDEX IF NOT EXISTS idx_artifacts_user_session "
	"  ON fcgi_artifacts(user_id, session_id);";

static int init_default_quota_profiles(sqlite3 *db)
{
	const char *sql = "INSERT OR IGNORE INTO fcgi_quota_profiles"
			  "(name,daily_turns,daily_images,daily_videos,"
			  "storage_bytes,max_concurrent_turns) VALUES"
			  "('admin',-1,-1,-1,-1,4),"
			  "('free',50,10,2,1073741824,1)";
	char *err = NULL;
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "fcgi quota profiles: %s\n", err ? err : "?");
		sqlite3_free(err);
		return NB_ERR_IO;
	}
	return 0;
}

struct session_store *session_store_open(struct nb_log *logger, const char *db_path)
{
	struct session_store *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;

	if (db_open(logger, &s->db, db_path) != 0) {
		free(s);
		return NULL;
	}
	if (db_init_schema(&s->db) != 0) {
		db_close(&s->db);
		free(s);
		return NULL;
	}

	char *err = NULL;
	if (sqlite3_exec(s->db.handle, SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "fcgi schema: %s\n", err ? err : "?");
		sqlite3_free(err);
		db_close(&s->db);
		free(s);
		return NULL;
	}
	if (init_default_quota_profiles(s->db.handle) != 0) {
		db_close(&s->db);
		free(s);
		return NULL;
	}
	(void)store_maintenance(s);

	s->mu = nb_mutex_new(0);
	s->cv = s->mu ? nb_cond_new() : NULL;
	if (!s->cv) {
		nb_mutex_free(s->mu);
		db_close(&s->db);
		free(s);
		return NULL;
	}
	return s;
}

void session_store_close(struct session_store *s)
{
	if (!s)
		return;
	store_events_cleanup(s);
	(nb_mutex_free(s->mu), s->mu = NULL);
	(nb_cond_free(s->cv), s->cv = NULL);
	db_close(&s->db);
	free(s);
}

int64_t fcgi_store_now(void)
{
	return (int64_t)time(NULL);
}

int store_transaction_begin(struct session_store *s)
{
	sqlite3_mutex *mutex = sqlite3_db_mutex(s->db.handle);

	/* Hold the FULLMUTEX connection lock across every transaction statement. */
	if (!mutex)
		return NB_ERR_UNSUPPORTED;
	sqlite3_mutex_enter(mutex);
	if (sqlite3_exec(s->db.handle, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
		sqlite3_mutex_leave(mutex);
		return NB_ERR_IO;
	}
	return 0;
}

int store_transaction_end(struct session_store *s, int rc)
{
	if (rc == 0 && sqlite3_exec(s->db.handle, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
		rc = NB_ERR_IO;
	if (rc != 0)
		(void)sqlite3_exec(s->db.handle, "ROLLBACK", NULL, NULL, NULL);
	sqlite3_mutex_leave(sqlite3_db_mutex(s->db.handle));
	return rc;
}

static int store_env_int(const char *name, int fallback)
{
	const char *value = getenv(name);
	char *end = NULL;
	long parsed;

	if (!value || !*value)
		return fallback;
	errno = 0;
	parsed = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed < 0)
		return fallback;
	return parsed > INT_MAX ? INT_MAX : (int)parsed;
}

int store_maintenance(struct session_store *s)
{
	int event_days = store_env_int("MORPH_FCGI_EVENT_RETENTION_DAYS", 7);
	int action_days = store_env_int("MORPH_FCGI_ACTION_RETENTION_DAYS", 1);
	int stale_seconds = store_env_int("MORPH_FCGI_TURN_STALE_SECONDS", 120);
	char sql[512];
	int64_t now = fcgi_store_now();

	if (!s)
		return NB_ERR_INVAL;
	snprintf(sql, sizeof(sql), "DELETE FROM fcgi_events WHERE ts < %lld",
		 (long long)(now - (int64_t)event_days * 86400));
	if (sqlite3_exec(s->db.handle, sql, NULL, NULL, NULL) != SQLITE_OK)
		return NB_ERR_IO;
	snprintf(sql, sizeof(sql), "DELETE FROM fcgi_actions WHERE consumed=1 AND ts < %lld",
		 (long long)(now - (int64_t)action_days * 86400));
	if (sqlite3_exec(s->db.handle, sql, NULL, NULL, NULL) != SQLITE_OK)
		return NB_ERR_IO;
	snprintf(sql, sizeof(sql), "DELETE FROM fcgi_running_turns WHERE heartbeat_at < %lld",
		 (long long)(now - stale_seconds));
	if (sqlite3_exec(s->db.handle, sql, NULL, NULL, NULL) != SQLITE_OK)
		return NB_ERR_IO;
	snprintf(sql, sizeof(sql),
		 "DELETE FROM fcgi_artifacts WHERE expires_at IS NOT NULL"
		 " AND expires_at < %lld",
		 (long long)now);
	if (sqlite3_exec(s->db.handle, sql, NULL, NULL, NULL) != SQLITE_OK)
		return NB_ERR_IO;
	return 0;
}
