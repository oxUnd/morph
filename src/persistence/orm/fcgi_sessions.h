#ifndef MORPH_FCGI_STORAGE_SESSIONS_H
#define MORPH_FCGI_STORAGE_SESSIONS_H

#include <stdint.h>

struct session_store;

/* sessions */
int store_create_session(struct session_store *s, const char *user_id, const char *name,
			 const char *model, char out_session_id[64]);
int store_session_owned_by(struct session_store *s, const char *session_id, const char *user_id);
int store_list_sessions_json(struct session_store *s, const char *user_id, char **out_json);
int store_list_sessions_page_json(struct session_store *s, const char *user_id, int limit,
				  int offset, char **out_json);
int store_update_session(struct session_store *s, const char *session_id, const char *user_id,
			 const char *name, const char *model);
int store_messages_json(struct session_store *s, const char *session_id, const char *user_id,
			int limit, int64_t before, char **out_json);
int store_delete_session(struct session_store *s, const char *session_id, const char *user_id);

#endif
