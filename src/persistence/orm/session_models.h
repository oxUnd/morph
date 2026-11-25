#ifndef MORPH_ORM_SESSION_MODELS_H
#define MORPH_ORM_SESSION_MODELS_H

#include "records.h"

enum session_field {
	SESSION_ID,
	SESSION_DISPLAY_ID,
	SESSION_NAME,
	SESSION_MODEL,
	SESSION_WORKDIR,
	SESSION_CREATED_AT,
	SESSION_UPDATED_AT,
	SESSION_TOKEN_USED,
};

struct session_row {
	int64_t id;
	nb_str display_id;
	bool display_id_missing;
	nb_str name;
	nb_str model;
	bool model_missing;
	nb_str workdir;
	int64_t created_at;
	int64_t updated_at;
	int64_t token_used;
	bool token_used_missing;
};

enum message_field {
	MESSAGE_ID,
	MESSAGE_SESSION_ID,
	MESSAGE_ROLE,
	MESSAGE_CONTENT,
	MESSAGE_TURN_ID,
	MESSAGE_TOKEN_COUNT,
	MESSAGE_COMPRESSED,
	MESSAGE_CREATED_AT,
};

struct message_row {
	int64_t id;
	int64_t session_id;
	nb_str role;
	nb_str content;
	bool content_missing;
	nb_str turn_id;
	bool turn_id_missing;
	int32_t token_count;
	int32_t compressed;
	bool compressed_missing;
	int64_t created_at;
};

extern const struct norm_model session_model;
extern const struct norm_model message_model;

#endif
