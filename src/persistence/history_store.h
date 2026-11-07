#ifndef MORPH_HISTORY_STORE_H
#define MORPH_HISTORY_STORE_H

#include "session.h"

struct arena;

#ifdef __cplusplus
extern "C" {
#endif

struct history_request_snapshot {
	const char *turn_id;
	const char *provider;
	const char *model;
	const char *input_json;
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

int history_store_context_tokens(struct db *db, int64_t session_id, int64_t *tokens);
int history_store_last_answer(struct db *db, int64_t session_id, char **answer);
int history_store_summary_sources(struct db *db, int64_t session_id, morph_array_t *ids);
int history_store_request_snapshot(struct db *db, int64_t session_id, struct arena *arena,
				   struct history_request_snapshot *snapshot);
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
int history_store_previous_question(struct db *db, int64_t session_id, int64_t before_sequence,
				    char **question);
int history_store_legacy_answer(struct db *db, int64_t session_id, struct arena *arena,
				const char **answer, const char **question);

#ifdef __cplusplus
}
#endif

#endif
