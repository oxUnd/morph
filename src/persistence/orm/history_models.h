#ifndef MORPH_ORM_HISTORY_MODELS_H
#define MORPH_ORM_HISTORY_MODELS_H

#include "records.h"
#include "session_history.h"

enum history_field {
	HIST_ID,
	HIST_SESSION_ID,
	HIST_SEQUENCE_NO,
	HIST_TURN_ID,
	HIST_KIND,
	HIST_ROLE,
	HIST_CONTENT,
	HIST_PAYLOAD_JSON,
	HIST_TOOL_CALL_ID,
	HIST_PROVIDER_CALL_ID,
	HIST_TOOL_NAME,
	HIST_IDEMPOTENCY_KEY,
	HIST_TOKEN_COUNT,
	HIST_TRUNCATED,
	HIST_ACTIVE,
	HIST_CREATED_AT,
};

struct history_row {
	int64_t id;
	int64_t session_id;
	int64_t sequence_no;
	nb_str turn_id;
	bool turn_id_missing;
	nb_str kind;
	nb_str role;
	bool role_missing;
	nb_str content;
	bool content_missing;
	nb_str payload_json;
	bool payload_json_missing;
	nb_str tool_call_id;
	bool tool_call_id_missing;
	nb_str provider_call_id;
	bool provider_call_id_missing;
	nb_str tool_name;
	bool tool_name_missing;
	nb_str idempotency_key;
	bool idempotency_key_missing;
	int token_count;
	int truncated;
	int active;
	int64_t created_at;
};

extern const struct norm_model history_model;
int orm_history_item_copy(const struct history_row *row, struct model_history_item *out);
void orm_history_item_cleanup(struct model_history_item *item);

#endif
