#ifndef MORPH_FCGI_STORAGE_EVENTS_H
#define MORPH_FCGI_STORAGE_EVENTS_H

#include <stdint.h>

struct session_store;

struct event_record {
	int64_t id;
	char type[32];
	char *payload_json;
	int64_t ts;
};

struct action_record {
	int64_t id;
	char type[32];
	char *payload_json;
	int64_t ts;
};

/* events */
int events_publish(struct session_store *s, const char *session_id, const char *type,
		   const char *json_payload);
int events_wait_after(struct session_store *s, const char *session_id, int64_t last_id,
		      int timeout_sec, struct event_record *rec);
int events_wait_batch(struct session_store *s, const char *session_id, int64_t last_id,
		      int timeout_sec, struct event_record *out, int max, int *count);
void store_wake_all(struct session_store *s);

/* actions */
int actions_enqueue(struct session_store *s, const char *session_id, const char *type,
		    const char *json_payload);
int actions_drain_one(struct session_store *s, const char *session_id, struct action_record *out);
void actions_signal(struct session_store *s, const char *session_id);
int actions_wait(struct session_store *s, const char *session_id, int timeout_sec);

#endif
