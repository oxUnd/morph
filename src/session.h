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
	char *turn_id;
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

#define MODEL_HISTORY_KIND_MAX 32
#define MODEL_HISTORY_ROLE_MAX 16
#define MODEL_HISTORY_TOOL_NAME_MAX 512

struct model_history_item {
	int64_t id;
	int64_t session_id;
	int64_t sequence_no;
	char *turn_id;
	char kind[MODEL_HISTORY_KIND_MAX];
	char role[MODEL_HISTORY_ROLE_MAX];
	char *content;
	char *payload_json;
	char *tool_call_id;
	char *provider_call_id;
	char tool_name[MODEL_HISTORY_TOOL_NAME_MAX];
	char *idempotency_key;
	int token_count;
	int truncated;
	int active;
	int64_t created_at;
	struct model_history_item *next;
};

struct model_history_insert {
	int64_t session_id;
	const char *turn_id;
	const char *kind;
	const char *role;
	const char *content;
	const char *payload_json;
	const char *tool_call_id;
	const char *provider_call_id;
	const char *tool_name;
	const char *idempotency_key;
	int token_count;
	int truncated;
	int active;
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
int message_add_with_turn_id(struct db *db, int64_t session_id,
			     const char *role, const char *content,
			     int token_count, const char *turn_id);
int message_delete(struct db *db, int64_t message_id);
struct message *message_list(struct db *db, int64_t session_id, int *count);
void message_free_list(struct message *head);
int message_count(struct db *db, int64_t session_id);

int model_history_add(struct db *db,
		      const struct model_history_insert *item,
		      int64_t *out_id);
struct model_history_item *model_history_list(struct db *db,
					       int64_t session_id,
					       int active_only,
					       int *count);
void model_history_free_list(struct model_history_item *head);
int model_history_count(struct db *db, int64_t session_id, int active_only);
int model_history_deactivate_turn(struct db *db, int64_t session_id,
				  const char *turn_id);
int model_history_compaction_count(struct db *db, int64_t session_id);
int model_history_compaction_attempt_add(struct db *db, int64_t session_id,
	const char *turn_id, const char *trigger_kind, const char *status,
	int input_tokens, int output_tokens, int error_code,
	const char *error_text);
int model_history_migrate_messages(struct db *db, int64_t session_id);
int model_history_compact(struct db *db, int64_t session_id,
			  const char *turn_id, const char *summary,
			  int summary_tokens, int user_message_tokens,
			  int recent_history_tokens, int input_tokens,
			  int keep_recent_rounds,
			  const char *trigger_kind,
			  int64_t *summary_item_id);

int trace_save(struct db *db, int64_t session_id, int round_no,
	       const char *steps_json, int aborted);
char *trace_load_latest(struct db *db, int64_t session_id,
			int *out_round_no, int *out_aborted);
int trace_get_next_round_no(struct db *db, int64_t session_id);

#ifdef __cplusplus
}
#endif

#endif
