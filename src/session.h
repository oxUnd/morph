#ifndef SESSION_H
#define SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

#include "db/database.h"
#include <limits.h>
#include <stdint.h>

struct session {
	int64_t id;
	char display_id[16];
	char name[256];
	char model[64];
	int64_t created_at;
	int64_t updated_at;
	int64_t token_used;
};

struct message {
	int64_t id;
	int64_t session_id;
	char role[32];
	char *content;
	int token_count;
	int compressed;
	int64_t created_at;
	struct message *next;
};

struct message_attachment {
	int64_t id;
	int64_t message_id;
	char kind[32];
	char path[PATH_MAX];
	char sha256[65];
};

int session_create(struct db *db, const char *name, const char *model, struct session *out);
int session_get_by_name(struct db *db, const char *name, struct session *out);
int session_get_by_id(struct db *db, int64_t id, struct session *out);
int session_get_by_display_id(struct db *db, const char *display_id, struct session *out);
int session_list(struct db *db, struct session **out, int *count,
		 int limit, const char *filter);
int session_count(struct db *db);
int session_rename(struct db *db, int64_t id, const char *new_name);
int session_delete(struct db *db, int64_t id);
int session_update_model(struct db *db, int64_t id, const char *model);
int session_update_tokens(struct db *db, int64_t id, int64_t added_tokens);

int session_ensure_display_id(struct db *db, struct session *s);

int message_add(struct db *db, int64_t session_id, const char *role,
		const char *content, int token_count);
int message_delete(struct db *db, int64_t message_id);
struct message *message_list(struct db *db, int64_t session_id, int *count);
void message_free_list(struct message *head);
int message_count(struct db *db, int64_t session_id);

int trace_save(struct db *db, int64_t session_id, int round_no,
	       const char *steps_json, int aborted);
char *trace_load_latest(struct db *db, int64_t session_id,
			int *out_round_no, int *out_aborted);
int trace_get_next_round_no(struct db *db, int64_t session_id);

#ifdef __cplusplus
}
#endif

#endif