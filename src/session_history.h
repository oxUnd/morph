#ifndef MORPH_SESSION_HISTORY_H
#define MORPH_SESSION_HISTORY_H

#include "util/array.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct db;

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

int model_history_add(struct db *db, const struct model_history_insert *item, int64_t *out_id);
/* Returns NULL with *count == 0 for an empty history, -1 on failure. */
struct model_history_item *model_history_list(struct db *db, int64_t session_id, int active_only,
					      int *count);
void model_history_free_list(struct model_history_item *head);
int model_history_count(struct db *db, int64_t session_id, int active_only);
int model_history_deactivate_turn(struct db *db, int64_t session_id, const char *turn_id);
int model_history_deactivate_turn_except_user(struct db *db, int64_t session_id,
					      const char *turn_id);
int model_history_compaction_count(struct db *db, int64_t session_id);
int model_history_compaction_attempt_add(struct db *db, int64_t session_id, const char *turn_id,
					 const char *trigger_kind, const char *status,
					 int input_tokens, int output_tokens, int error_code,
					 const char *error_text);
int model_history_migrate_messages(struct db *db, int64_t session_id);
/* Recent rounds and request/answer pairs are retained within their budgets. */
/* User retention is capped; unused user budget is available to recent history. */
int model_history_compact(struct db *db, int64_t session_id, const char *turn_id,
			  const char *summary, int summary_tokens, int user_message_tokens,
			  int recent_history_tokens, int input_tokens, int keep_recent_rounds,
			  const char *trigger_kind, int64_t *summary_item_id);

/* Freeze retention before generating a summary; commit exactly that selection. */
int model_history_select_retained(struct db *db, int64_t session_id, int user_message_tokens,
				  int recent_history_tokens, int keep_recent_rounds,
				  morph_array_t *preserved);
int model_history_compact_selected(struct db *db, int64_t session_id, const char *turn_id,
				   const char *summary, int summary_tokens, int input_tokens,
				   const char *trigger_kind, int64_t *summary_item_id,
				   const morph_array_t *preserved, int64_t cutoff);

#ifdef __cplusplus
}
#endif

#endif
