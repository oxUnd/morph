#include "persistence/orm/fcgi_store.h"
#include "persistence/orm/fcgi_events.h"
#include "sapi/fastcgi/session_store_internal.h"
#include "persistence/orm/fcgi_models.h"
#include "persistence/orm/records.h"
#include <nb/time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct event_subscriber {
	char session_id[64];
	nb_cond *cv;
	struct event_subscriber *next;
};

static int event_insert(struct session_store *store, const char *session_id, const char *type,
			const char *payload, bool action)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	int rc = store ? orm_scope_begin(&store->db, &scope) : NB_ERR_INVAL;

	if (rc != 0)
		return rc;

	if (action) {
		const struct orm_fcgi_actions_row row = {
			.session_id = nb_str_from_cstr(session_id),
			.type = nb_str_from_cstr(type),
			.payload = nb_str_from_cstr(payload ? payload : "{}"),
			.ts = fcgi_store_now(),
		};

		return orm_status(&store->db,
				  norm_insert(scope.connection, &orm_fcgi_actions, &row, NULL,
					      orm_scope_call(&scope), NULL, &error),
				  &error);
	}

	const struct orm_fcgi_events_row row = {
		.session_id = nb_str_from_cstr(session_id),
		.type = nb_str_from_cstr(type),
		.payload = nb_str_from_cstr(payload ? payload : "{}"),
		.ts = fcgi_store_now(),
	};

	return orm_status(&store->db,
			  norm_insert(scope.connection, &orm_fcgi_events, &row, NULL,
				      orm_scope_call(&scope), NULL, &error),
			  &error);
}

int events_publish(struct session_store *store, const char *session_id, const char *type,
		   const char *payload)
{
	int rc = event_insert(store, session_id, type, payload, false);

	if (rc != 0)
		return rc;

	nb_mutex_lock(store->mu);
	for (struct event_subscriber *subscriber = store->subs; subscriber;
	     subscriber = subscriber->next) {
		if (session_id && strcmp(subscriber->session_id, session_id) == 0)
			nb_cond_signal(subscriber->cv);
	}

	nb_mutex_unlock(store->mu);
	return 0;
}

static int events_fetch_batch(struct session_store *store, const char *session_id, int64_t last_id,
			      int limit, struct event_record *out, int *count)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const struct norm_expr predicates[] = {
		norm_eq(norm_col(&orm_fcgi_events, 1), orm_string(session_id)),
		norm_gt(norm_col(&orm_fcgi_events, 0), norm_i64(last_id)),
	};
	const struct norm_order order = {
		.column = norm_col(&orm_fcgi_events, 0),
		.direction = NORM_ASC,
	};
	const struct norm_select_query query = {
		.where = norm_and(predicates, 2),
		.order = &order,
		.order_count = 1,
		.has_limit = true,
		.limit = (uint64_t)limit,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};
	int rc = orm_scope_begin(&store->db, &scope);

	*count = 0;
	if (rc == 0)
		rc = orm_status(&store->db,
				norm_select(scope.connection, &orm_fcgi_events, &query,
					    orm_scope_call(&scope), &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	size_t filled = 0;

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct orm_fcgi_events_row *row = norm_rows_at(rows, i);

		memset(&out[i], 0, sizeof(out[i]));
		out[i].id = row->id;
		out[i].ts = row->ts;
		rc = nb_str_copy_to(row->type, out[i].type, sizeof(out[i].type));
		if (rc == 0) {
			out[i].payload_json = orm_text_copy(row->payload);
			if (!out[i].payload_json)
				rc = NB_ERR_NOMEM;
		}

		filled++;
	}

	if (rc != 0) {
		for (size_t i = 0; i < filled; i++) {
			free(out[i].payload_json);
			out[i].payload_json = NULL;
		}

		return rc;
	}

	*count = (int)filled;
	return 0;
}

int actions_enqueue(struct session_store *store, const char *session_id, const char *type,
		    const char *payload)
{
	int rc = event_insert(store, session_id, type, payload, true);

	if (rc == 0)
		actions_signal(store, session_id);

	return rc;
}

static int action_drain(struct session_store *store, const char *session_id,
			struct action_record *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	int rc = orm_scope_begin(&store->db, &scope);
	const struct norm_expr predicates[] = {
		norm_eq(norm_col(&orm_fcgi_actions, 1), orm_string(session_id)),
		norm_eq(norm_col(&orm_fcgi_actions, 5), norm_i64(0)),
	};
	const struct norm_order order = {
		.column = norm_col(&orm_fcgi_actions, 0),
		.direction = NORM_ASC,
	};
	const struct norm_select_query query = {
		.where = norm_and(predicates, 2),
		.order = &order,
		.order_count = 1,
		.has_limit = true,
		.limit = 1,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};

	if (rc == 0)
		rc = orm_status(&store->db,
				norm_select(scope.connection, &orm_fcgi_actions, &query,
					    orm_scope_call(&scope), &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	const struct orm_fcgi_actions_row *row = norm_rows_at(rows, 0);

	if (!row)
		return NB_ERR_NO_DATA;

	struct action_record value = {.id = row->id, .ts = row->ts};

	rc = nb_str_copy_to(row->type, value.type, sizeof(value.type));
	if (rc != 0)
		return rc;

	value.payload_json = orm_text_copy(row->payload);
	if (!value.payload_json)
		return NB_ERR_NOMEM;

	const struct norm_assignment consumed[] = {
		{.column = norm_col(&orm_fcgi_actions, 5), .value = norm_i64(1)},
	};
	const struct norm_update_query consume = {
		.set = consumed,
		.set_count = 1,
		.where = norm_eq(norm_col(&orm_fcgi_actions, 0), norm_i64(value.id)),
	};

	rc = orm_status(&store->db,
			norm_update(scope.connection, &orm_fcgi_actions, &consume,
				    orm_scope_call(&scope), NULL, &error),
			&error);
	if (rc != 0) {
		free(value.payload_json);
		return rc;
	}

	*out = value;
	return 0;
}

int actions_drain_one(struct session_store *store, const char *session_id,
		      struct action_record *out)
{
	if (!store || !session_id || !out || fcgi_store_transaction_begin(store) != 0)
		return 0;

	int rc = action_drain(store, session_id, out);
	int finish = fcgi_store_transaction_end(store, rc);

	if (rc == 0 && finish != 0) {
		free(out->payload_json);
		out->payload_json = NULL;
	}

	return rc == 0 && finish == 0;
}

int events_wait_batch(struct session_store *s, const char *session_id, int64_t last_id,
		      int timeout_sec, struct event_record *out, int max, int *count)
{
	struct event_subscriber *sub;
	struct event_subscriber **pp;
	int rc;

	if (!s || !session_id || !out || !count || max <= 0)
		return NB_ERR_INVAL;
	*count = 0;
	nb_mutex_lock(s->mu);
	sub = calloc(1, sizeof(*sub));
	if (!sub) {
		nb_mutex_unlock(s->mu);
		return NB_ERR_NOMEM;
	}
	snprintf(sub->session_id, sizeof(sub->session_id), "%s", session_id);
	if (((sub->cv = nb_cond_new()) ? NB_OK : NB_ERR_NOMEM) != 0) {
		free(sub);
		nb_mutex_unlock(s->mu);
		return NB_ERR_IO;
	}
	sub->next = s->subs;
	s->subs = sub;

	rc = events_fetch_batch(s, session_id, last_id, max, out, count);
	if (rc == 0 && *count == 0 && timeout_sec > 0) {
		rc = nb_cond_timedwait(sub->cv, s->mu, (int64_t)timeout_sec * INT64_C(1000000000));
		if (rc == NB_ERR_TIMEOUT)
			rc = 0;
		else if (rc != 0)
			rc = NB_ERR_IO;
		else
			rc = events_fetch_batch(s, session_id, last_id, max, out, count);
	}
	for (pp = &s->subs; *pp; pp = &(*pp)->next) {
		if (*pp == sub) {
			*pp = sub->next;
			break;
		}
	}
	nb_mutex_unlock(s->mu);
	(nb_cond_free(sub->cv), sub->cv = NULL);
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

void actions_signal(struct session_store *s, const char *session_id)
{
	(void)session_id;
	nb_mutex_lock(s->mu);
	nb_cond_broadcast(s->cv);
	nb_mutex_unlock(s->mu);
}

void store_wake_all(struct session_store *s)
{
	if (!s)
		return;
	nb_mutex_lock(s->mu);
	for (struct event_subscriber *sub = s->subs; sub; sub = sub->next)
		nb_cond_signal(sub->cv);
	nb_cond_broadcast(s->cv);
	nb_mutex_unlock(s->mu);
}

int actions_wait(struct session_store *s, const char *session_id, int timeout_sec)
{
	(void)session_id;
	nb_mutex_lock(s->mu);
	int rc = nb_cond_timedwait(s->cv, s->mu, (int64_t)timeout_sec * INT64_C(1000000000));
	nb_mutex_unlock(s->mu);
	return rc == NB_ERR_TIMEOUT ? 0 : rc == NB_OK ? 1 : rc;
}

void store_events_cleanup(struct session_store *s)
{
	struct event_subscriber *subscriber;

	nb_mutex_lock(s->mu);
	subscriber = s->subs;
	while (subscriber) {
		struct event_subscriber *next = subscriber->next;

		(nb_cond_free(subscriber->cv), subscriber->cv = NULL);
		free(subscriber);
		subscriber = next;
	}
	s->subs = NULL;
	nb_mutex_unlock(s->mu);
}
