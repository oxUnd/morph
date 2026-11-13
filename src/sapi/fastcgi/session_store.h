#ifndef MORPH_FCGI_SESSION_STORE_H
#define MORPH_FCGI_SESSION_STORE_H

#include "db/database.h"
#include "nb/thread.h"

struct nb_log;
struct event_subscriber;

/* One shared SQLite connection; domain modules borrow this owner. */
struct session_store {
	struct db db;
	nb_mutex *mu;
	nb_cond *cv;
	struct event_subscriber *subs;
};

struct session_store *session_store_open(struct nb_log *logger, const char *db_path);
void session_store_close(struct session_store *store);
int store_maintenance(struct session_store *store);

#endif
