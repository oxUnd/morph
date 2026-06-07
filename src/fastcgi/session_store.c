/* session_store.c — SQLite-backed multi-session store with cond-var fanout */
#define _GNU_SOURCE
#include "session_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sqlite3.h>

#include "session.h"
#include "util/buf.h"
#include "util/error.h"
#include "security.h"
#include "cJSON.h"

struct event_subscriber {
	char     session_id[64];
	int64_t  last_seen;
	struct event_subscriber *next;
};

static const char *SCHEMA =
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
	const char *sql =
		"INSERT OR IGNORE INTO fcgi_quota_profiles"
		"(name,daily_turns,daily_images,daily_videos,"
		"storage_bytes,max_concurrent_turns) VALUES"
		"('admin',-1,-1,-1,-1,4),"
		"('free',50,10,2,1073741824,1)";
	char *err = NULL;
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "fcgi quota profiles: %s\n", err ? err : "?");
		sqlite3_free(err);
		MORPH_RETURN(-EIO);
	}
	return 0;
}

struct session_store *session_store_open(const char *db_path) {
	struct session_store *s = calloc(1, sizeof(*s));
	if (!s) return NULL;

	if (db_open(&s->db, db_path) != 0) { free(s); return NULL; }
	if (db_init_schema(&s->db) != 0)   { db_close(&s->db); free(s); return NULL; }

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

	pthread_mutex_init(&s->mu, NULL);
	pthread_cond_init(&s->cv, NULL);
	return s;
}

void session_store_close(struct session_store *s) {
	if (!s) return;
	pthread_mutex_lock(&s->mu);
	struct event_subscriber *p = s->subs;
	while (p) { struct event_subscriber *n = p->next; free(p); p = n; }
	pthread_mutex_unlock(&s->mu);
	pthread_mutex_destroy(&s->mu);
	pthread_cond_destroy(&s->cv);
	db_close(&s->db);
	free(s);
}

static int64_t now_unix(void) { return (int64_t)time(NULL); }

static int username_valid(const char *username)
{
	size_t len;

	if (!username)
		return 0;
	len = strlen(username);
	if (len < 3 || len > 63)
		return 0;
	for (size_t i = 0; i < len; i++) {
		char c = username[i];
		if (!((c >= 'a' && c <= 'z') ||
		      (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') ||
		      c == '_' || c == '-' || c == '.')) {
			return 0;
		}
	}
	return 1;
}

int store_setup_required(struct session_store *s)
{
	const char *sql =
		"SELECT COUNT(*) FROM fcgi_users WHERE disabled=0";
	sqlite3_stmt *st = NULL;
	int count = 0;

	if (!s || sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return 1;
	if (sqlite3_step(st) == SQLITE_ROW)
		count = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return count == 0;
}

int store_create_user(struct session_store *s, const char *username,
		      const char *password, const char *role,
		      char out_user_id[64])
{
	char user_id[64];
	char hash[192];
	const char *profile;
	const char *sql =
		"INSERT INTO fcgi_users"
		"(user_id,username,password_hash,role,created_at,updated_at)"
		" VALUES(?,?,?,?,?,?)";
	sqlite3_stmt *st = NULL;
	int64_t now = now_unix();
	int rc;

	if (!s || !username_valid(username) || !password || strlen(password) < 8)
		MORPH_RETURN(-EINVAL);
	rc = fcgi_random_id("usr_", user_id, sizeof(user_id));
	if (rc < 0)
		return rc;
	rc = fcgi_password_hash(password, hash, sizeof(hash));
	if (rc < 0)
		return rc;

	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		MORPH_RETURN(-EIO);
	sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, username, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, hash, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 4, role ? role : "user", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 5, now);
	sqlite3_bind_int64(st, 6, now);
	rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc == SQLITE_CONSTRAINT)
		MORPH_RETURN(-EEXIST);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(-EIO);

	profile = role && strcmp(role, "admin") == 0 ? "admin" : "free";
	sql = "INSERT OR REPLACE INTO fcgi_user_quota(user_id,profile)"
	      " VALUES(?,?)";
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		MORPH_RETURN(-EIO);
	sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, profile, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(-EIO);

	sql = "INSERT OR IGNORE INTO fcgi_usage_storage(user_id,used_bytes)"
	      " VALUES(?,0)";
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
		sqlite3_step(st);
		sqlite3_finalize(st);
	}
	if (out_user_id)
		snprintf(out_user_id, 64, "%s", user_id);
	return 0;
}

int store_verify_user(struct session_store *s, const char *username,
		      const char *password, struct fcgi_user *out)
{
	const char *sql =
		"SELECT user_id,username,password_hash,role FROM fcgi_users "
		"WHERE username=? AND disabled=0";
	sqlite3_stmt *st = NULL;
	const char *hash;
	int ok = 0;

	if (!s || !username || !password)
		return 0;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return 0;
	sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(st) == SQLITE_ROW) {
		hash = (const char *)sqlite3_column_text(st, 2);
		if (hash && fcgi_password_verify(password, hash)) {
			if (out) {
				const char *uid =
					(const char *)sqlite3_column_text(st, 0);
				const char *uname =
					(const char *)sqlite3_column_text(st, 1);
				const char *role =
					(const char *)sqlite3_column_text(st, 3);
				snprintf(out->user_id, sizeof(out->user_id),
					 "%s", uid ? uid : "");
				snprintf(out->username, sizeof(out->username),
					 "%s", uname ? uname : "");
				snprintf(out->role, sizeof(out->role),
					 "%s", role ? role : "user");
			}
			ok = 1;
		}
	}
	sqlite3_finalize(st);
	return ok;
}

static void add_limit_obj(cJSON *parent, const char *name, int used, int limit)
{
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddNumberToObject(obj, "used", used);
	cJSON_AddNumberToObject(obj, "limit", limit);
	cJSON_AddItemToObject(parent, name, obj);
}

int store_user_quota_json(struct session_store *s, const char *user_id,
			  char **out_json)
{
	const char *sql =
		"SELECT q.profile,p.daily_turns,p.daily_images,p.daily_videos,"
		"p.storage_bytes,p.max_concurrent_turns,"
		"COALESCE(d.turns,0),COALESCE(d.images,0),"
		"COALESCE(d.videos,0),COALESCE(st.used_bytes,0) "
		"FROM fcgi_user_quota q "
		"JOIN fcgi_quota_profiles p ON p.name=q.profile "
		"LEFT JOIN fcgi_usage_daily d ON d.user_id=q.user_id "
		" AND d.day=date('now') "
		"LEFT JOIN fcgi_usage_storage st ON st.user_id=q.user_id "
		"WHERE q.user_id=?";
	sqlite3_stmt *stmt = NULL;
	cJSON *root = NULL;
	cJSON *daily = NULL;
	cJSON *storage = NULL;
	cJSON *conc = NULL;
	char *json = NULL;

	if (!s || !user_id || !out_json)
		MORPH_RETURN(-EINVAL);
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		MORPH_RETURN(-EIO);
	sqlite3_bind_text(stmt, 1, user_id, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) != SQLITE_ROW) {
		sqlite3_finalize(stmt);
		MORPH_RETURN(-ENOENT);
	}

	root = cJSON_CreateObject();
	daily = cJSON_CreateObject();
	storage = cJSON_CreateObject();
	conc = cJSON_CreateObject();
	if (!root || !daily || !storage || !conc) {
		cJSON_Delete(root);
		sqlite3_finalize(stmt);
		MORPH_RETURN(-ENOMEM);
	}
	cJSON_AddStringToObject(root, "profile",
		(const char *)sqlite3_column_text(stmt, 0));
	add_limit_obj(daily, "turns", sqlite3_column_int(stmt, 6),
		      sqlite3_column_int(stmt, 1));
	add_limit_obj(daily, "images", sqlite3_column_int(stmt, 7),
		      sqlite3_column_int(stmt, 2));
	add_limit_obj(daily, "videos", sqlite3_column_int(stmt, 8),
		      sqlite3_column_int(stmt, 3));
	cJSON_AddItemToObject(root, "daily", daily);
	cJSON_AddNumberToObject(storage, "used_bytes",
				(double)sqlite3_column_int64(stmt, 9));
	cJSON_AddNumberToObject(storage, "limit_bytes",
				(double)sqlite3_column_int64(stmt, 4));
	cJSON_AddItemToObject(root, "storage", storage);
	cJSON_AddNumberToObject(conc, "used", 0);
	cJSON_AddNumberToObject(conc, "limit", sqlite3_column_int(stmt, 5));
	cJSON_AddItemToObject(root, "concurrent_turns", conc);

	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	sqlite3_finalize(stmt);
	if (!json)
		MORPH_RETURN(-ENOMEM);
	*out_json = json;
	return 0;
}

int store_quota_begin_turn(struct session_store *s, const char *user_id,
			   const char *session_id, char out_turn_id[64])
{
	const char *qsql =
		"SELECT p.daily_turns,p.max_concurrent_turns,"
		"COALESCE(d.turns,0),"
		"(SELECT COUNT(*) FROM fcgi_running_turns WHERE user_id=?) "
		"FROM fcgi_user_quota q "
		"JOIN fcgi_quota_profiles p ON p.name=q.profile "
		"LEFT JOIN fcgi_usage_daily d ON d.user_id=q.user_id "
		" AND d.day=date('now') WHERE q.user_id=?";
	const char *usql =
		"INSERT INTO fcgi_usage_daily(user_id,day,turns)"
		" VALUES(?,date('now'),1) "
		"ON CONFLICT(user_id,day) DO UPDATE SET turns=turns+1";
	const char *isql =
		"INSERT INTO fcgi_running_turns"
		"(id,user_id,session_id,started_at,heartbeat_at)"
		" VALUES(?,?,?,?,?)";
	sqlite3_stmt *st = NULL;
	char turn_id[64];
	int daily_limit;
	int concurrent_limit;
	int used_turns;
	int running;
	int rc;
	int64_t now = now_unix();

	if (!s || !user_id || !session_id)
		MORPH_RETURN(-EINVAL);
	rc = fcgi_random_id("turn_", turn_id, sizeof(turn_id));
	if (rc < 0)
		return rc;

	sqlite3_exec(s->db.handle, "BEGIN IMMEDIATE", NULL, NULL, NULL);
	if (sqlite3_prepare_v2(s->db.handle, qsql, -1, &st, NULL) != SQLITE_OK)
		goto fail;
	sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, user_id, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(st) != SQLITE_ROW) {
		sqlite3_finalize(st);
		sqlite3_exec(s->db.handle, "ROLLBACK", NULL, NULL, NULL);
		MORPH_RETURN(-ENOENT);
	}
	daily_limit = sqlite3_column_int(st, 0);
	concurrent_limit = sqlite3_column_int(st, 1);
	used_turns = sqlite3_column_int(st, 2);
	running = sqlite3_column_int(st, 3);
	sqlite3_finalize(st);

	if (daily_limit >= 0 && used_turns >= daily_limit) {
		sqlite3_exec(s->db.handle, "ROLLBACK", NULL, NULL, NULL);
		MORPH_RETURN(-EDQUOT);
	}
	if (concurrent_limit >= 0 && running >= concurrent_limit) {
		sqlite3_exec(s->db.handle, "ROLLBACK", NULL, NULL, NULL);
		MORPH_RETURN(-EAGAIN);
	}

	if (sqlite3_prepare_v2(s->db.handle, usql, -1, &st, NULL) != SQLITE_OK)
		goto fail;
	sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		goto fail;

	if (sqlite3_prepare_v2(s->db.handle, isql, -1, &st, NULL) != SQLITE_OK)
		goto fail;
	sqlite3_bind_text(st, 1, turn_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, user_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 4, now);
	sqlite3_bind_int64(st, 5, now);
	rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		goto fail;

	sqlite3_exec(s->db.handle, "COMMIT", NULL, NULL, NULL);
	if (out_turn_id)
		snprintf(out_turn_id, 64, "%s", turn_id);
	return 0;

fail:
	sqlite3_exec(s->db.handle, "ROLLBACK", NULL, NULL, NULL);
	MORPH_RETURN(-EIO);
}

void store_quota_end_turn(struct session_store *s, const char *turn_id)
{
	const char *sql = "DELETE FROM fcgi_running_turns WHERE id=?";
	sqlite3_stmt *st = NULL;

	if (!s || !turn_id || !*turn_id)
		return;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, turn_id, -1, SQLITE_TRANSIENT);
		sqlite3_step(st);
		sqlite3_finalize(st);
	}
}

static char *artifact_json_array(sqlite3_stmt *st)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *items = cJSON_CreateArray();
	char *json;

	if (!root || !items) {
		cJSON_Delete(root);
		return NULL;
	}
	cJSON_AddItemToObject(root, "items", items);
	while (sqlite3_step(st) == SQLITE_ROW) {
		cJSON *obj = cJSON_CreateObject();
		const char *id = (const char *)sqlite3_column_text(st, 0);
		char url[128];
		if (!obj) {
			cJSON_Delete(root);
			return NULL;
		}
		snprintf(url, sizeof(url), "/api/artifacts/%s", id ? id : "");
		cJSON_AddStringToObject(obj, "id", id ? id : "");
		cJSON_AddStringToObject(obj, "session_id",
			(const char *)sqlite3_column_text(st, 1));
		cJSON_AddStringToObject(obj, "kind",
			(const char *)sqlite3_column_text(st, 2));
		cJSON_AddStringToObject(obj, "mime",
			(const char *)sqlite3_column_text(st, 3));
		cJSON_AddStringToObject(obj, "filename",
			(const char *)sqlite3_column_text(st, 4));
		cJSON_AddNumberToObject(obj, "size_bytes",
			(double)sqlite3_column_int64(st, 5));
		cJSON_AddStringToObject(obj, "status",
			(const char *)sqlite3_column_text(st, 6));
		cJSON_AddNumberToObject(obj, "created_at",
			(double)sqlite3_column_int64(st, 7));
		cJSON_AddStringToObject(obj, "url", url);
		cJSON_AddItemToArray(items, obj);
	}
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return json;
}

int store_artifact_register(struct session_store *s, const char *user_id,
			    const char *session_id, const char *kind,
			    const char *mime, const char *filename,
			    const char *relative_path, int64_t size_bytes,
			    char out_artifact_id[64])
{
	const char *sql =
		"INSERT INTO fcgi_artifacts"
		"(id,user_id,session_id,kind,mime,filename,relative_path,"
		"size_bytes,status,created_at)"
		" VALUES(?,?,?,?,?,?,?,?, 'ready', ?)";
	sqlite3_stmt *st = NULL;
	char id[64];
	int rc;

	if (!s || !user_id || !session_id || !kind || !mime ||
	    !filename || !relative_path)
		MORPH_RETURN(-EINVAL);
	rc = fcgi_random_id("art_", id, sizeof(id));
	if (rc < 0)
		return rc;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		MORPH_RETURN(-EIO);
	sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, user_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 4, kind, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 5, mime, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 6, filename, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 7, relative_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 8, size_bytes);
	sqlite3_bind_int64(st, 9, now_unix());
	rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(-EIO);

	sql = "INSERT INTO fcgi_usage_storage(user_id,used_bytes)"
	      " VALUES(?,?) "
	      "ON CONFLICT(user_id) DO UPDATE SET "
	      "used_bytes=used_bytes+excluded.used_bytes";
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(st, 2, size_bytes);
		sqlite3_step(st);
		sqlite3_finalize(st);
	}
	if (strcmp(kind, "image") == 0 || strcmp(kind, "video") == 0) {
		const char *col = strcmp(kind, "image") == 0 ? "images" : "videos";
		char usage_sql[256];
		snprintf(usage_sql, sizeof(usage_sql),
			"INSERT INTO fcgi_usage_daily(user_id,day,%s)"
			" VALUES(?,date('now'),1) "
			"ON CONFLICT(user_id,day) DO UPDATE SET %s=%s+1",
			col, col, col);
		if (sqlite3_prepare_v2(s->db.handle, usage_sql, -1,
				       &st, NULL) == SQLITE_OK) {
			sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
			sqlite3_step(st);
			sqlite3_finalize(st);
		}
	}
	if (out_artifact_id)
		snprintf(out_artifact_id, 64, "%s", id);
	return 0;
}

int store_artifact_get(struct session_store *s, const char *artifact_id,
		       const char *user_id, struct artifact_record *out)
{
	const char *sql =
		"SELECT id,user_id,session_id,kind,mime,filename,"
		"relative_path,size_bytes,status,created_at "
		"FROM fcgi_artifacts WHERE id=? AND user_id=?";
	sqlite3_stmt *st = NULL;
	int found = 0;

	if (!s || !artifact_id || !user_id || !out)
		MORPH_RETURN(-EINVAL);
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		MORPH_RETURN(-EIO);
	sqlite3_bind_text(st, 1, artifact_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, user_id, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(st) == SQLITE_ROW) {
		memset(out, 0, sizeof(*out));
		snprintf(out->id, sizeof(out->id), "%s",
			(const char *)sqlite3_column_text(st, 0));
		snprintf(out->user_id, sizeof(out->user_id), "%s",
			(const char *)sqlite3_column_text(st, 1));
		snprintf(out->session_id, sizeof(out->session_id), "%s",
			(const char *)sqlite3_column_text(st, 2));
		snprintf(out->kind, sizeof(out->kind), "%s",
			(const char *)sqlite3_column_text(st, 3));
		snprintf(out->mime, sizeof(out->mime), "%s",
			(const char *)sqlite3_column_text(st, 4));
		snprintf(out->filename, sizeof(out->filename), "%s",
			(const char *)sqlite3_column_text(st, 5));
		snprintf(out->relative_path, sizeof(out->relative_path), "%s",
			(const char *)sqlite3_column_text(st, 6));
		out->size_bytes = sqlite3_column_int64(st, 7);
		snprintf(out->status, sizeof(out->status), "%s",
			(const char *)sqlite3_column_text(st, 8));
		out->created_at = sqlite3_column_int64(st, 9);
		found = 1;
	}
	sqlite3_finalize(st);
	return found ? 0 : -ENOENT;
}

int store_artifact_list_json(struct session_store *s, const char *user_id,
			     const char *session_id, char **out_json)
{
	const char *sql =
		"SELECT id,session_id,kind,mime,filename,size_bytes,status,"
		"created_at FROM fcgi_artifacts "
		"WHERE user_id=? AND session_id=? ORDER BY created_at DESC";
	sqlite3_stmt *st = NULL;
	char *json;

	if (!s || !user_id || !session_id || !out_json)
		MORPH_RETURN(-EINVAL);
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		MORPH_RETURN(-EIO);
	sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, session_id, -1, SQLITE_TRANSIENT);
	json = artifact_json_array(st);
	sqlite3_finalize(st);
	if (!json)
		MORPH_RETURN(-ENOMEM);
	*out_json = json;
	return 0;
}

int store_create_session(struct session_store *s, const char *user_id,
			 const char *name, const char *model,
			 char out_session_id[64]) {
	struct session sess;
	if (session_create(&s->db, name, model, &sess) != 0) MORPH_RETURN(-EIO);
	if (session_ensure_display_id(&s->db, &sess) != 0) MORPH_RETURN(-EIO);
	snprintf(out_session_id, 64, "%s", sess.display_id);

	const char *sql = "INSERT OR REPLACE INTO fcgi_session_owner(session_id,user_id) VALUES(?,?)";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK) MORPH_RETURN(-EIO);
	sqlite3_bind_text(st, 1, out_session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, user_id,        -1, SQLITE_TRANSIENT);
	int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	return rc == SQLITE_DONE ? 0 : -EIO;
}

int store_session_owned_by(struct session_store *s, const char *session_id,
			   const char *user_id) {
	const char *sql = "SELECT 1 FROM fcgi_session_owner WHERE session_id=? AND user_id=?";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK) return 0;
	sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, user_id,    -1, SQLITE_TRANSIENT);
	int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	return rc == SQLITE_ROW ? 1 : 0;
}

int store_list_sessions_json(struct session_store *s, const char *user_id,
			     char **out_json) {
	const char *sql =
		"SELECT s.display_id, s.name, s.model, s.created_at, s.updated_at "
		"FROM fcgi_session_owner o "
		"JOIN sessions s ON s.display_id = o.session_id "
		"WHERE o.user_id=? ORDER BY s.updated_at DESC";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK) MORPH_RETURN(-EIO);
	sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);

	morph_buf_t buf;
	if (morph_buf_init(&buf, 1024) < 0) { sqlite3_finalize(st); MORPH_RETURN(-ENOMEM); }
	if (morph_buf_puts(&buf, "{\"items\":[") != 0) {
		morph_buf_cleanup(&buf);
		sqlite3_finalize(st);
		MORPH_RETURN(-ENOMEM);
	}

	int first = 1;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *id    = (const char *)sqlite3_column_text(st, 0);
		const char *name  = (const char *)sqlite3_column_text(st, 1);
		const char *model = (const char *)sqlite3_column_text(st, 2);
		int64_t ca = sqlite3_column_int64(st, 3);
		int64_t ua = sqlite3_column_int64(st, 4);

		if (morph_buf_printf(&buf,
			"%s{\"id\":\"%s\",\"name\":\"%s\",\"model\":\"%s\","
			"\"created_at\":%lld,\"updated_at\":%lld}",
			first ? "" : ",",
			id ? id : "", name ? name : "", model ? model : "",
			(long long)ca, (long long)ua) != 0) {
			morph_buf_cleanup(&buf);
			sqlite3_finalize(st);
			MORPH_RETURN(-ENOMEM);
		}
		first = 0;
	}
	sqlite3_finalize(st);
	if (morph_buf_puts(&buf, "]}") != 0) {
		morph_buf_cleanup(&buf);
		MORPH_RETURN(-ENOMEM);
	}
	*out_json = morph_buf_detach(&buf);
	return 0;
}

int store_delete_session(struct session_store *s, const char *session_id,
			const char *user_id)
{
	static const char *chk_owner =
		"SELECT 1 FROM fcgi_session_owner"
		" WHERE session_id=? AND user_id=?";
	static const char *chk_turns =
		"SELECT COUNT(*) FROM fcgi_running_turns WHERE session_id=?";
	static const char *del_tables[] = {
		"DELETE FROM fcgi_session_owner WHERE session_id=?",
		"DELETE FROM fcgi_events WHERE session_id=?",
		"DELETE FROM fcgi_actions WHERE session_id=?",
		"DELETE FROM fcgi_canvas_nodes WHERE session_id=?",
		"DELETE FROM fcgi_artifacts WHERE session_id=?",
		"DELETE FROM fcgi_running_turns WHERE session_id=?",
		NULL
	};
	sqlite3_stmt *st = NULL;
	struct session sess;
	int rc;

	if (!s || !session_id || !user_id)
		MORPH_RETURN(-EINVAL);

	sqlite3_exec(s->db.handle, "BEGIN IMMEDIATE", NULL, NULL, NULL);

	if (sqlite3_prepare_v2(s->db.handle, chk_owner, -1,
			       &st, NULL) != SQLITE_OK)
		goto fail;
	sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, user_id,    -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_ROW) {
		sqlite3_exec(s->db.handle, "ROLLBACK", NULL, NULL, NULL);
		MORPH_RETURN(-ENOENT);
	}

	if (sqlite3_prepare_v2(s->db.handle, chk_turns, -1,
			       &st, NULL) != SQLITE_OK)
		goto fail;
	sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(st);
	int running = (rc == SQLITE_ROW) ? sqlite3_column_int(st, 0) : 0;
	sqlite3_finalize(st);
	if (running > 0) {
		sqlite3_exec(s->db.handle, "ROLLBACK", NULL, NULL, NULL);
		MORPH_RETURN(-EAGAIN);
	}

	for (int i = 0; del_tables[i]; i++) {
		if (sqlite3_prepare_v2(s->db.handle, del_tables[i], -1,
				       &st, NULL) != SQLITE_OK)
			goto fail;
		sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
		rc = sqlite3_step(st);
		sqlite3_finalize(st);
		if (rc != SQLITE_DONE)
			goto fail;
	}

	if (session_get_by_display_id(&s->db, session_id, &sess) != 0)
		goto fail;
	if (session_delete(&s->db, sess.id) != 0)
		goto fail;

	sqlite3_exec(s->db.handle, "COMMIT", NULL, NULL, NULL);
	return 0;

fail:
	sqlite3_exec(s->db.handle, "ROLLBACK", NULL, NULL, NULL);
	MORPH_RETURN(-EIO);
}

int events_publish(struct session_store *s, const char *session_id,
		   const char *type, const char *payload) {
	const char *sql = "INSERT INTO fcgi_events(session_id,type,payload,ts) VALUES(?,?,?,?)";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK) MORPH_RETURN(-EIO);
	sqlite3_bind_text (st, 1, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (st, 2, type,       -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (st, 3, payload ? payload : "{}", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 4, now_unix());
	int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE) MORPH_RETURN(-EIO);

	pthread_mutex_lock(&s->mu);
	pthread_cond_broadcast(&s->cv);
	pthread_mutex_unlock(&s->mu);
	return 0;
}

static int events_fetch_after(struct session_store *s, const char *session_id,
			      int64_t last_id, struct event_record *rec) {
	const char *sql =
		"SELECT id,type,payload,ts FROM fcgi_events "
		"WHERE session_id=? AND id>? ORDER BY id LIMIT 1";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK) MORPH_RETURN(-EIO);
	sqlite3_bind_text (st, 1, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 2, last_id);

	int rc = sqlite3_step(st);
	if (rc == SQLITE_ROW) {
		rec->id = sqlite3_column_int64(st, 0);
		const char *t = (const char *)sqlite3_column_text(st, 1);
		const char *p = (const char *)sqlite3_column_text(st, 2);
		snprintf(rec->type, sizeof(rec->type), "%s", t ? t : "message");
		rec->payload_json = strdup(p ? p : "{}");
		rec->ts = sqlite3_column_int64(st, 3);
		sqlite3_finalize(st);
		return 1;
	}
	sqlite3_finalize(st);
	return 0;
}

int events_wait_after(struct session_store *s, const char *session_id,
		      int64_t last_id, int timeout_sec,
		      struct event_record *rec) {
	int got = events_fetch_after(s, session_id, last_id, rec);
	if (got != 0) return got;

	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += timeout_sec;

	pthread_mutex_lock(&s->mu);
	int rc = pthread_cond_timedwait(&s->cv, &s->mu, &deadline);
	pthread_mutex_unlock(&s->mu);
	if (rc == ETIMEDOUT) return 0;

	return events_fetch_after(s, session_id, last_id, rec);
}

int actions_enqueue(struct session_store *s, const char *session_id,
		    const char *type, const char *payload) {
	const char *sql = "INSERT INTO fcgi_actions(session_id,type,payload,ts) VALUES(?,?,?,?)";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK) MORPH_RETURN(-EIO);
	sqlite3_bind_text (st, 1, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (st, 2, type,       -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (st, 3, payload ? payload : "{}", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 4, now_unix());
	int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE) MORPH_RETURN(-EIO);

	actions_signal(s, session_id);
	return 0;
}

int actions_drain_one(struct session_store *s, const char *session_id,
		      struct action_record *out) {
	sqlite3_exec(s->db.handle, "BEGIN IMMEDIATE", NULL, NULL, NULL);

	const char *qsql =
		"SELECT id,type,payload,ts FROM fcgi_actions "
		"WHERE session_id=? AND consumed=0 ORDER BY id LIMIT 1";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, qsql, -1, &st, NULL) != SQLITE_OK) {
		sqlite3_exec(s->db.handle, "ROLLBACK", NULL, NULL, NULL);
		return 0;
	}
	sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
	int rc = sqlite3_step(st);
	int got = 0;
	if (rc == SQLITE_ROW) {
		out->id = sqlite3_column_int64(st, 0);
		const char *t = (const char *)sqlite3_column_text(st, 1);
		const char *p = (const char *)sqlite3_column_text(st, 2);
		snprintf(out->type, sizeof(out->type), "%s", t ? t : "");
		out->payload_json = strdup(p ? p : "{}");
		out->ts = sqlite3_column_int64(st, 3);
		got = 1;
	}
	sqlite3_finalize(st);

	if (got) {
		const char *usql = "UPDATE fcgi_actions SET consumed=1 WHERE id=?";
		sqlite3_stmt *us = NULL;
		if (sqlite3_prepare_v2(s->db.handle, usql, -1, &us, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(us, 1, out->id);
			sqlite3_step(us);
			sqlite3_finalize(us);
		}
	}
	sqlite3_exec(s->db.handle, "COMMIT", NULL, NULL, NULL);
	return got;
}

void actions_signal(struct session_store *s, const char *session_id) {
	(void)session_id;
	pthread_mutex_lock(&s->mu);
	pthread_cond_broadcast(&s->cv);
	pthread_mutex_unlock(&s->mu);
}

int actions_wait(struct session_store *s, const char *session_id,
		 int timeout_sec) {
	(void)session_id;
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += timeout_sec;
	pthread_mutex_lock(&s->mu);
	int rc = pthread_cond_timedwait(&s->cv, &s->mu, &deadline);
	pthread_mutex_unlock(&s->mu);
	return rc == ETIMEDOUT ? 0 : 1;
}

int canvas_list_json(struct session_store *s, const char *session_id,
		     char **out_json) {
	const char *sql =
		"SELECT id,parent_id,kind,x,y,w,h,content_ref,meta,updated_at "
		"FROM fcgi_canvas_nodes WHERE session_id=? ORDER BY updated_at";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK) MORPH_RETURN(-EIO);
	sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);

	morph_buf_t buf;
	if (morph_buf_init(&buf, 2048) < 0) { sqlite3_finalize(st); MORPH_RETURN(-ENOMEM); }
	if (morph_buf_puts(&buf, "{\"nodes\":[") != 0) {
		morph_buf_cleanup(&buf);
		sqlite3_finalize(st);
		MORPH_RETURN(-ENOMEM);
	}
	int first = 1;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *id     = (const char *)sqlite3_column_text(st, 0);
		const char *parent = (const char *)sqlite3_column_text(st, 1);
		const char *kind   = (const char *)sqlite3_column_text(st, 2);
		double x = sqlite3_column_double(st, 3);
		double y = sqlite3_column_double(st, 4);
		double w = sqlite3_column_double(st, 5);
		double h = sqlite3_column_double(st, 6);
		const char *cref = (const char *)sqlite3_column_text(st, 7);
		const char *meta = (const char *)sqlite3_column_text(st, 8);
		int64_t up = sqlite3_column_int64(st, 9);
		if (morph_buf_printf(&buf,
			"%s{\"id\":\"%s\",\"parent\":%s%s%s,\"kind\":\"%s\","
			"\"x\":%g,\"y\":%g,\"w\":%g,\"h\":%g,"
			"\"content_ref\":%s%s%s,\"meta\":%s,\"updated_at\":%lld}",
			first ? "" : ",",
			id ? id : "",
			parent ? "\"" : "", parent ? parent : "null", parent ? "\"" : "",
			kind ? kind : "",
			x, y, w, h,
			cref ? "\"" : "", cref ? cref : "null", cref ? "\"" : "",
			meta && *meta ? meta : "null",
			(long long)up) != 0) {
			morph_buf_cleanup(&buf);
			sqlite3_finalize(st);
			MORPH_RETURN(-ENOMEM);
		}
		first = 0;
	}
	sqlite3_finalize(st);
	if (morph_buf_puts(&buf, "]}") != 0) {
		morph_buf_cleanup(&buf);
		MORPH_RETURN(-ENOMEM);
	}
	*out_json = morph_buf_detach(&buf);
	return 0;
}

static void mk_node_id(char out[40]) {
	int64_t t = now_unix();
	snprintf(out, 40, "n_%llx_%x",
		 (long long)t, (unsigned)(rand() & 0xffffff));
}

int canvas_add_node(struct session_store *s, const char *session_id,
		    const char *node_json, char out_node_id[40]) {
	mk_node_id(out_node_id);
	const char *sql =
		"INSERT INTO fcgi_canvas_nodes(id,session_id,kind,x,y,w,h,meta,created_at,updated_at) "
		"VALUES(?,?,'node',0,0,0,0,?,?,?)";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK) MORPH_RETURN(-EIO);
	sqlite3_bind_text (st, 1, out_node_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (st, 2, session_id,  -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (st, 3, node_json ? node_json : "{}", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 4, now_unix());
	sqlite3_bind_int64(st, 5, now_unix());
	int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	return rc == SQLITE_DONE ? 0 : -EIO;
}

int canvas_patch_node(struct session_store *s, const char *session_id,
		      const char *node_id, const char *patch_json) {
	const char *sql =
		"UPDATE fcgi_canvas_nodes SET meta=?, updated_at=? "
		"WHERE id=? AND session_id=?";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK) MORPH_RETURN(-EIO);
	sqlite3_bind_text (st, 1, patch_json ? patch_json : "{}", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 2, now_unix());
	sqlite3_bind_text (st, 3, node_id,    -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (st, 4, session_id, -1, SQLITE_TRANSIENT);
	int rc = sqlite3_step(st);
	int changes = sqlite3_changes(s->db.handle);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE) MORPH_RETURN(-EIO);
	return changes > 0 ? 0 : -ENOENT;
}

/* ---- login tokens (/tmp file-backed) ---- */

#define SESS_DIR "/tmp/morph-sess"
#define SESS_TOKEN_TTL_HOURS 24

static int ensure_sess_dir(void)
{
	struct stat st;
	if (stat(SESS_DIR, &st) == 0 && S_ISDIR(st.st_mode))
		return 0;
	if (mkdir(SESS_DIR, 0700) != 0 && errno != EEXIST)
		MORPH_RETURN_ERRNO();
	return 0;
}

int login_token_create(const char *user_id, const char *username,
		       const char *role, int ttl_hours,
		       char out_token[64])
{
	char token[64];
	char path[PATH_MAX];
	int64_t now = now_unix();
	int64_t expires = now + (int64_t)(ttl_hours > 0 ? ttl_hours : SESS_TOKEN_TTL_HOURS) * 3600;
	cJSON *obj = NULL;
	char *json = NULL;
	FILE *fp = NULL;
	int rc;

	rc = ensure_sess_dir();
	if (rc < 0) return rc;
	rc = fcgi_random_id("sess_", token, sizeof(token));
	if (rc < 0) return rc;

	obj = cJSON_CreateObject();
	if (!obj) MORPH_RETURN(-ENOMEM);
	cJSON_AddStringToObject(obj, "user_id", user_id ? user_id : "");
	cJSON_AddStringToObject(obj, "username", username ? username : "");
	cJSON_AddStringToObject(obj, "role", role ? role : "user");
	cJSON_AddNumberToObject(obj, "expires_at", (double)expires);
	json = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	if (!json) MORPH_RETURN(-ENOMEM);

	snprintf(path, sizeof(path), "%s/%s.json", SESS_DIR, token);
	fp = fopen(path, "wx");
	if (!fp) {
		int err = errno;
		free(json);
		MORPH_RETURN(-err);
	}
	fwrite(json, 1, strlen(json), fp);
	fclose(fp);
	free(json);

	snprintf(out_token, 64, "%s", token);
	return 0;
}

int login_token_verify(const char *token,
		       char out_user_id[64],
		       char out_username[64],
		       char out_role[24])
{
	char path[PATH_MAX];
	char buf[1024];
	FILE *fp;
	size_t n;
	cJSON *obj = NULL;
	const char *s;
	int64_t expires;
	int valid = 0;

	if (!token || !*token) return 0;
	if (strchr(token, '/') || strstr(token, "..")) return 0;
	snprintf(path, sizeof(path), "%s/%s.json", SESS_DIR, token);

	fp = fopen(path, "r");
	if (!fp) return 0;
	n = fread(buf, 1, sizeof(buf) - 1, fp);
	fclose(fp);
	if (n == 0) return 0;
	buf[n] = '\0';

	obj = cJSON_Parse(buf);
	if (!obj) return 0;

	expires = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(obj, "expires_at"));
	if (expires > now_unix()) {
		valid = 1;
		s = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "user_id"));
		if (s && out_user_id) snprintf(out_user_id, 64, "%s", s);
		s = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "username"));
		if (s && out_username) snprintf(out_username, 64, "%s", s);
		s = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "role"));
		if (s && out_role) snprintf(out_role, 24, "%s", s);
	}
	cJSON_Delete(obj);

	if (!valid) {
		unlink(path);
		return 0;
	}
	return 1;
}

void login_token_revoke(const char *token)
{
	char path[PATH_MAX];
	if (!token || !*token) return;
	if (strchr(token, '/') || strstr(token, "..")) return;
	snprintf(path, sizeof(path), "%s/%s.json", SESS_DIR, token);
	unlink(path);
}
