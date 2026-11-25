#ifndef MORPH_FCGI_SESSION_STORE_H
#define MORPH_FCGI_SESSION_STORE_H

#include "persistence/orm/database.h"
#include <nb/thread.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nb_log;
struct event_subscriber;

/* Persistence owner and event-observation state. */
struct session_store {
	struct db db;
	nb_mutex *mu;
	nb_cond *cv;
	struct event_subscriber *subs;
};

struct session_store *session_store_open(struct nb_log *logger, const char *db_path);
struct session_store *session_store_open_options(struct nb_log *logger,
						const struct db_options *options);
void session_store_close(struct session_store *store);
int store_maintenance(struct session_store *store);

#ifdef __cplusplus
}
#endif

#endif
