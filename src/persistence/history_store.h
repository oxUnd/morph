#ifndef MORPH_HISTORY_STORE_H
#define MORPH_HISTORY_STORE_H

#include "session.h"

#ifdef __cplusplus
extern "C" {
#endif

struct history_message_progress {
	int64_t latest_id;
	int completed_turns;
};

struct history_request_snapshot {
	char *turn_id;
	char *provider;
	char *model;
	char *input_json;
	int64_t created_at;
};

struct history_display_record {
	const char *type;
	const char *content;
	const char *attachments_json;
	const char *structured_data;
	const char *agent_ui_ir;
	const char *turn_id;
	int64_t hitl_verdict;
	int64_t core_message_id;
	int64_t created_at;
	unsigned int present;
};

#define HISTORY_DISPLAY_VERDICT (1u << 0)
#define HISTORY_DISPLAY_MESSAGE (1u << 1)
#define HISTORY_DISPLAY_CREATED (1u << 2)

struct history_trace_record {
	int64_t id;
	const char *steps_json;
	int64_t created_at;
};

/* Visitor records are borrowed until the callback returns. Negative results
 * abort; a positive result stops iteration successfully. */
typedef int (*history_item_visitor)(const struct model_history_item *item, void *data);
typedef int (*history_message_visitor)(const struct message *message, void *data);
typedef int (*history_display_visitor)(const struct history_display_record *record, void *data);
typedef int (*history_trace_visitor)(const struct history_trace_record *record, void *data);

int history_store_visible_progress(struct db *db, int64_t session_id,
				   struct history_message_progress *out);
/* Recent visible user/assistant messages, newest first, by insertion identity. */
int history_store_visit_recent_messages(struct db *db, int64_t session_id, int limit,
					history_message_visitor visitor, void *data);
int history_store_update_item(struct db *db, int64_t id, int active, int token_count);
/* Policies supply token counts; storage owns retained-row mapping and writes. */
typedef int (*history_token_counter)(const struct model_history_item *item, void *data);
int history_store_refresh_retention_tokens(struct db *db, int64_t session_id,
					   history_token_counter count, void *data);
int history_store_add_summary_sources(struct db *db, int64_t summary_id, const int64_t *ids,
				      size_t count);
int history_store_last_sequence(struct db *db, int64_t session_id, int64_t *sequence);
int history_store_context_tokens(struct db *db, int64_t session_id, int64_t *tokens);
/* Text copies require a NULL output slot and return malloc-owned strings.
 * Failure preserves an occupied owner, or leaves an empty slot NULL. */
int history_store_last_answer_copy(struct db *db, int64_t session_id, char **answer);
int history_store_summary_sources(struct db *db, int64_t session_id, morph_array_t *ids);
/* Outputs must be zero-initialized and empty. Occupied owners return -EEXIST. */
int history_store_request_snapshot(struct db *db, int64_t session_id,
				   struct history_request_snapshot *snapshot);
void history_request_snapshot_cleanup(struct history_request_snapshot *snapshot);
int history_store_visit_messages(struct db *db, int64_t session_id, history_message_visitor visitor,
				 void *data);
/* An absent imported display table returns -ENODATA. */
int history_store_visit_display(struct db *db, int64_t session_id, history_display_visitor visitor,
				void *data);
int history_store_visit_traces(struct db *db, int64_t session_id, history_trace_visitor visitor,
			       void *data);
int history_store_visit_answers(struct db *db, int64_t session_id, history_item_visitor visitor,
				void *data);
int history_store_visit_turn(struct db *db, int64_t session_id, const char *turn_id,
			     int64_t before_sequence, history_item_visitor visitor, void *data);
int history_store_previous_question_copy(struct db *db, int64_t session_id, int64_t before_sequence,
					 char **question);
/* Both outputs must be empty and receive independent malloc-owned strings.
 * Failure preserves occupied owners, or leaves both empty outputs NULL. */
int history_store_legacy_answer_copy(struct db *db, int64_t session_id, char **answer,
				     char **question);

#ifdef __cplusplus
}
#endif

#endif
