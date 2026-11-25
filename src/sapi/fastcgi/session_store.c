#include "sapi/fastcgi/session_store.h"
#include "sapi/fastcgi/session_store_internal.h"
#include "persistence/orm/fcgi_events.h"
#include "persistence/orm/fcgi_store.h"

#include <stdlib.h>
#include <time.h>

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

	if (orm_fcgi_schema(&s->db) != 0) {
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
