/* session_store.c — SQLite-backed multi-session store with cond-var fanout */
#define _GNU_SOURCE
#include "session_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>

#include "session.h"
#include "util/error.h"

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
");";

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

	size_t cap = 1024, len = 0;
	char *buf = malloc(cap);
	if (!buf) { sqlite3_finalize(st); MORPH_RETURN(-ENOMEM); }
	len += (size_t)snprintf(buf + len, cap - len, "{\"items\":[");

	int first = 1;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *id    = (const char *)sqlite3_column_text(st, 0);
		const char *name  = (const char *)sqlite3_column_text(st, 1);
		const char *model = (const char *)sqlite3_column_text(st, 2);
		int64_t ca = sqlite3_column_int64(st, 3);
		int64_t ua = sqlite3_column_int64(st, 4);

		while (cap - len < 512) {
			cap *= 2;
			char *tmp = realloc(buf, cap);
			if (!tmp) { free(buf); sqlite3_finalize(st); MORPH_RETURN(-ENOMEM); }
			buf = tmp;
		}
		len += (size_t)snprintf(buf + len, cap - len,
			"%s{\"id\":\"%s\",\"name\":\"%s\",\"model\":\"%s\","
			"\"created_at\":%lld,\"updated_at\":%lld}",
			first ? "" : ",",
			id ? id : "", name ? name : "", model ? model : "",
			(long long)ca, (long long)ua);
		first = 0;
	}
	sqlite3_finalize(st);
	while (cap - len < 4) {
		cap *= 2;
		char *tmp = realloc(buf, cap);
		if (!tmp) { free(buf); MORPH_RETURN(-ENOMEM); }
		buf = tmp;
	}
	len += (size_t)snprintf(buf + len, cap - len, "]}");
	*out_json = buf;
	return 0;
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

	size_t cap = 2048, len = 0;
	char *buf = malloc(cap);
	if (!buf) { sqlite3_finalize(st); MORPH_RETURN(-ENOMEM); }
	len += (size_t)snprintf(buf + len, cap - len, "{\"nodes\":[");
	int first = 1;
	while (sqlite3_step(st) == SQLITE_ROW) {
		while (cap - len < 1024) {
			cap *= 2;
			char *tmp = realloc(buf, cap);
			if (!tmp) { free(buf); sqlite3_finalize(st); MORPH_RETURN(-ENOMEM); }
			buf = tmp;
		}
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
		len += (size_t)snprintf(buf + len, cap - len,
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
			(long long)up);
		first = 0;
	}
	sqlite3_finalize(st);
	while (cap - len < 4) {
		cap *= 2;
		char *tmp = realloc(buf, cap);
		if (!tmp) { free(buf); MORPH_RETURN(-ENOMEM); }
		buf = tmp;
	}
	len += (size_t)snprintf(buf + len, cap - len, "]}");
	*out_json = buf;
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
