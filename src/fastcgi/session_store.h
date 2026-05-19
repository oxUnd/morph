/* session_store.h — multi-session backing for morph-fastcgi
 *
 * Wraps the existing morph SQLite database (src/db, src/session) and adds
 * three FastCGI-specific tables:
 *   - fcgi_events       : append-only event log (Agent → Web)
 *   - fcgi_actions      : pending commands (Web → Agent)
 *   - fcgi_canvas_nodes : per-session canvas nodes
 *
 * Multi-process safe via SQLite WAL mode.  morph CLI and morph-fastcgi can
 * share the same DB file.
 */
#ifndef MORPH_FCGI_SESSION_STORE_H
#define MORPH_FCGI_SESSION_STORE_H

#include <pthread.h>
#include <stdint.h>

#include "db/database.h"

struct event_subscriber;

struct session_store {
	struct db db;
	pthread_mutex_t mu;
	pthread_cond_t  cv;
	struct event_subscriber *subs;
};

struct event_record {
	int64_t id;
	char    type[32];
	char   *payload_json;
	int64_t ts;
};

struct action_record {
	int64_t id;
	char    type[32];
	char   *payload_json;
	int64_t ts;
};

/* lifecycle */
struct session_store *session_store_open(const char *db_path);
void                  session_store_close(struct session_store *s);

/* sessions */
int  store_create_session(struct session_store *s, const char *user_id,
			  const char *name, const char *model,
			  char out_session_id[64]);
int  store_session_owned_by(struct session_store *s, const char *session_id,
			    const char *user_id);
int  store_list_sessions_json(struct session_store *s, const char *user_id,
			      char **out_json);

/* events */
int  events_publish(struct session_store *s, const char *session_id,
		    const char *type, const char *json_payload);
int  events_wait_after(struct session_store *s, const char *session_id,
		       int64_t last_id, int timeout_sec,
		       struct event_record *rec);

/* actions */
int  actions_enqueue(struct session_store *s, const char *session_id,
		     const char *type, const char *json_payload);
int  actions_drain_one(struct session_store *s, const char *session_id,
		       struct action_record *out);
void actions_signal(struct session_store *s, const char *session_id);
int  actions_wait(struct session_store *s, const char *session_id,
		  int timeout_sec);

/* canvas */
int  canvas_list_json(struct session_store *s, const char *session_id,
		      char **out_json);
int  canvas_add_node(struct session_store *s, const char *session_id,
		     const char *node_json, char out_node_id[40]);
int  canvas_patch_node(struct session_store *s, const char *session_id,
		       const char *node_id, const char *patch_json);

#endif
