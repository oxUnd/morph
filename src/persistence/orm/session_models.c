#include "session_models.h"

static const struct norm_field session_fields[] = {
	NORM_FIELD(struct session_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_NULLABLE_TEXT_FIELD(struct session_row, display_id, display_id_missing, 0, 0),
	NORM_TEXT_FIELD(struct session_row, name, 0, 0),
	NORM_NULLABLE_TEXT_FIELD(struct session_row, model, model_missing, 0, 0),
	NORM_TEXT_FIELD(struct session_row, workdir, 0, 0),
	NORM_FIELD(struct session_row, created_at, NORM_TYPE_I64, 0),
	NORM_FIELD(struct session_row, updated_at, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct session_row, token_used, token_used_missing, NORM_TYPE_I64, 0),
};

const struct norm_model session_model = NORM_MODEL(struct session_row, "sessions", session_fields);

static const struct norm_field message_fields[] = {
	NORM_FIELD(struct message_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_FIELD(struct message_row, session_id, NORM_TYPE_I64, 0),
	NORM_TEXT_FIELD(struct message_row, role, 0, 0),
	NORM_NULLABLE_TEXT_FIELD(struct message_row, content, content_missing, 0, 0),
	NORM_NULLABLE_TEXT_FIELD(struct message_row, turn_id, turn_id_missing, 0, 0),
	NORM_FIELD(struct message_row, token_count, NORM_TYPE_I32, 0),
	NORM_NULLABLE_FIELD(struct message_row, compressed, compressed_missing, NORM_TYPE_I32, 0),
	NORM_FIELD(struct message_row, created_at, NORM_TYPE_I64, 0),
};

const struct norm_model message_model = NORM_MODEL(struct message_row, "messages", message_fields);
