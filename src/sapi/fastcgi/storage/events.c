#include "sapi/fastcgi/storage/events.h"
#include "sapi/fastcgi/session_store_internal.h"
#include "cJSON.h"
#include "util/error.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>

struct event_subscriber {
	char session_id[64];
	pthread_cond_t cv;
	struct event_subscriber *next;
};

int events_publish(struct session_store *s, const char *session_id, const char *type,
		   const char *payload)
{
	const char *sql = "INSERT INTO fcgi_events(session_id,type,payload,ts) VALUES(?,?,?,?)";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, type, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, payload ? payload : "{}", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 4, fcgi_store_now());
	int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return -EIO;

	pthread_mutex_lock(&s->mu);
	for (struct event_subscriber *sub = s->subs; sub; sub = sub->next) {
		if (session_id && strcmp(sub->session_id, session_id) == 0)
			pthread_cond_signal(&sub->cv);
	}
	pthread_mutex_unlock(&s->mu);
	return 0;
}

static int events_fetch_batch(struct session_store *s, const char *session_id, int64_t last_id,
			      int limit, struct event_record *out, int *count)
{
	const char *sql = "SELECT id,type,payload,ts FROM fcgi_events "
			  "WHERE session_id=? AND id>? ORDER BY id LIMIT ?";
	sqlite3_stmt *st = NULL;
	int n = 0;

	*count = 0;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 2, last_id);
	sqlite3_bind_int(st, 3, limit);

	for (;;) {
		int rc = sqlite3_step(st);
		const char *t;
		const char *p;

		if (rc == SQLITE_DONE)
			break;
		if (rc != SQLITE_ROW) {
			for (int i = 0; i < n; i++)
				free(out[i].payload_json);
			sqlite3_finalize(st);
			return -EIO;
		}
		memset(&out[n], 0, sizeof(out[n]));
		out[n].id = sqlite3_column_int64(st, 0);
		t = (const char *)sqlite3_column_text(st, 1);
		p = (const char *)sqlite3_column_text(st, 2);
		snprintf(out[n].type, sizeof(out[n].type), "%s", t ? t : "message");
		out[n].payload_json = strdup(p ? p : "{}");
		out[n].ts = sqlite3_column_int64(st, 3);
		if (!out[n].payload_json) {
			for (int i = 0; i <= n; i++)
				free(out[i].payload_json);
			sqlite3_finalize(st);
			return -ENOMEM;
		}
		n++;
	}
	sqlite3_finalize(st);
	*count = n;
	return 0;
}

int events_wait_batch(struct session_store *s, const char *session_id, int64_t last_id,
		      int timeout_sec, struct event_record *out, int max, int *count)
{
	struct event_subscriber *sub;
	struct event_subscriber **pp;
	struct timespec deadline;
	int rc;

	if (!s || !session_id || !out || !count || max <= 0)
		return -EINVAL;
	*count = 0;
	pthread_mutex_lock(&s->mu);
	sub = calloc(1, sizeof(*sub));
	if (!sub) {
		pthread_mutex_unlock(&s->mu);
		return -ENOMEM;
	}
	snprintf(sub->session_id, sizeof(sub->session_id), "%s", session_id);
	if (pthread_cond_init(&sub->cv, NULL) != 0) {
		free(sub);
		pthread_mutex_unlock(&s->mu);
		return -EIO;
	}
	sub->next = s->subs;
	s->subs = sub;

	rc = events_fetch_batch(s, session_id, last_id, max, out, count);
	if (rc == 0 && *count == 0 && timeout_sec > 0) {
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += timeout_sec;
		rc = pthread_cond_timedwait(&sub->cv, &s->mu, &deadline);
		if (rc == ETIMEDOUT)
			rc = 0;
		else if (rc != 0)
			rc = -EIO;
		else
			rc = events_fetch_batch(s, session_id, last_id, max, out, count);
	}
	for (pp = &s->subs; *pp; pp = &(*pp)->next) {
		if (*pp == sub) {
			*pp = sub->next;
			break;
		}
	}
	pthread_mutex_unlock(&s->mu);
	pthread_cond_destroy(&sub->cv);
	free(sub);
	return rc;
}

int events_wait_after(struct session_store *s, const char *session_id, int64_t last_id,
		      int timeout_sec, struct event_record *rec)
{
	int count = 0;
	int rc = events_wait_batch(s, session_id, last_id, timeout_sec, rec, 1, &count);

	if (rc < 0)
		return rc;
	return count == 0 ? 0 : 1;
}

int actions_enqueue(struct session_store *s, const char *session_id, const char *type,
		    const char *payload)
{
	const char *sql = "INSERT INTO fcgi_actions(session_id,type,payload,ts) VALUES(?,?,?,?)";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, type, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, payload ? payload : "{}", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 4, fcgi_store_now());
	int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return -EIO;

	actions_signal(s, session_id);
	return 0;
}

int actions_drain_one(struct session_store *s, const char *session_id, struct action_record *out)
{
	if (store_transaction_begin(s) != 0)
		return 0;

	const char *qsql = "SELECT id,type,payload,ts FROM fcgi_actions "
			   "WHERE session_id=? AND consumed=0 ORDER BY id LIMIT 1";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, qsql, -1, &st, NULL) != SQLITE_OK) {
		(void)store_transaction_end(s, -EIO);
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
	if (store_transaction_end(s, 0) != 0) {
		if (got) {
			free(out->payload_json);
			out->payload_json = NULL;
		}
		return 0;
	}
	return got;
}

void actions_signal(struct session_store *s, const char *session_id)
{
	(void)session_id;
	pthread_mutex_lock(&s->mu);
	pthread_cond_broadcast(&s->cv);
	pthread_mutex_unlock(&s->mu);
}

void store_wake_all(struct session_store *s)
{
	if (!s)
		return;
	pthread_mutex_lock(&s->mu);
	for (struct event_subscriber *sub = s->subs; sub; sub = sub->next)
		pthread_cond_signal(&sub->cv);
	pthread_cond_broadcast(&s->cv);
	pthread_mutex_unlock(&s->mu);
}

int actions_wait(struct session_store *s, const char *session_id, int timeout_sec)
{
	(void)session_id;
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += timeout_sec;
	pthread_mutex_lock(&s->mu);
	int rc = pthread_cond_timedwait(&s->cv, &s->mu, &deadline);
	pthread_mutex_unlock(&s->mu);
	return rc == ETIMEDOUT ? 0 : 1;
}

void store_events_cleanup(struct session_store *s)
{
	struct event_subscriber *subscriber;

	pthread_mutex_lock(&s->mu);
	subscriber = s->subs;
	while (subscriber) {
		struct event_subscriber *next = subscriber->next;

		pthread_cond_destroy(&subscriber->cv);
		free(subscriber);
		subscriber = next;
	}
	s->subs = NULL;
	pthread_mutex_unlock(&s->mu);
}
