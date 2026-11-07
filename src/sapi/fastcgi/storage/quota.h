#ifndef MORPH_FCGI_STORAGE_QUOTA_H
#define MORPH_FCGI_STORAGE_QUOTA_H

#include <stdint.h>

struct session_store;

int store_user_quota_json(struct session_store *s, const char *user_id, char **out_json);
int store_quota_begin_turn(struct session_store *s, const char *user_id, const char *session_id,
			   char out_turn_id[64]);
void store_quota_end_turn(struct session_store *s, const char *turn_id);
void store_turn_heartbeat(struct session_store *s, const char *turn_id);

#endif
