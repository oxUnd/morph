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

struct artifact_record {
	char id[64];
	char user_id[64];
	char session_id[64];
	char kind[16];
	char mime[64];
	char filename[128];
	char relative_path[512];
	int64_t size_bytes;
	char status[16];
	int64_t created_at;
};

struct fcgi_user {
	char user_id[64];
	char username[64];
	char role[24];
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

/* users / setup */
int  store_setup_required(struct session_store *s);
int  store_create_user(struct session_store *s, const char *username,
		       const char *password, const char *role,
		       char out_user_id[64]);
int  store_verify_user(struct session_store *s, const char *username,
		       const char *password, struct fcgi_user *out);
int  store_user_quota_json(struct session_store *s, const char *user_id,
			   char **out_json);
int  store_quota_begin_turn(struct session_store *s, const char *user_id,
			    const char *session_id, char out_turn_id[64]);
void store_quota_end_turn(struct session_store *s, const char *turn_id);

/* artifacts */
int  store_artifact_register(struct session_store *s, const char *user_id,
			     const char *session_id, const char *kind,
			     const char *mime, const char *filename,
			     const char *relative_path, int64_t size_bytes,
			     char out_artifact_id[64]);
int  store_artifact_get(struct session_store *s, const char *artifact_id,
			const char *user_id, struct artifact_record *out);
int  store_artifact_list_json(struct session_store *s, const char *user_id,
			      const char *session_id, char **out_json);

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

/* login tokens (/tmp file-backed) */
int  login_token_create(const char *user_id, const char *username,
			const char *role, int ttl_hours,
			char out_token[64]);
int  login_token_verify(const char *token,
			char out_user_id[64],
			char out_username[64],
			char out_role[24]);
void login_token_revoke(const char *token);

#endif
