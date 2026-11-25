#include "history_models.h"

static const struct norm_field history_fields[] = {
	NORM_FIELD(struct history_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_FIELD(struct history_row, session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct history_row, sequence_no, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct history_row, turn_id, turn_id_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct history_row, kind, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct history_row, role, role_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct history_row, content, content_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct history_row, payload_json, payload_json_missing, NORM_TYPE_TEXT,
			    0),
	NORM_NULLABLE_FIELD(struct history_row, tool_call_id, tool_call_id_missing, NORM_TYPE_TEXT,
			    0),
	NORM_NULLABLE_FIELD(struct history_row, provider_call_id, provider_call_id_missing,
			    NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct history_row, tool_name, tool_name_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct history_row, idempotency_key, idempotency_key_missing,
			    NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct history_row, token_count, NORM_TYPE_I32, 0),
	NORM_FIELD(struct history_row, truncated, NORM_TYPE_I32, 0),
	NORM_FIELD(struct history_row, active, NORM_TYPE_I32, 0),
	NORM_FIELD(struct history_row, created_at, NORM_TYPE_I64, 0),
};
const struct norm_model history_model =
	NORM_MODEL(struct history_row, "model_history_items", history_fields);

void orm_history_item_cleanup(struct model_history_item *item)
{
	free(item->turn_id);
	free(item->content);
	free(item->payload_json);
	free(item->tool_call_id);
	free(item->provider_call_id);
	free(item->tool_name);
	free(item->idempotency_key);
	memset(item, 0, sizeof(*item));
}

int orm_history_item_copy(const struct history_row *row, struct model_history_item *out)
{
	struct model_history_item value NB_CLEANUP(orm_history_item_cleanup) = {0};

	if (!row || !out)
		return NB_ERR_INVAL;

	value.id = row->id;
	value.session_id = row->session_id;
	value.sequence_no = row->sequence_no;
	value.token_count = row->token_count;
	value.truncated = row->truncated;
	value.active = row->active;
	value.created_at = row->created_at;
	if (nb_str_copy_to(row->kind.ptr ? row->kind : NB_STR_LIT(""), value.kind,
			   sizeof(value.kind)) != NB_OK ||
	    nb_str_copy_to(row->role_missing ? NB_STR_LIT("") : row->role, value.role,
			   sizeof(value.role)) != NB_OK)
		return NB_ERR_RANGE;

	value.tool_name = orm_text_copy(row->tool_name_missing ? NB_STR_LIT("") : row->tool_name);
	if (!value.tool_name)
		return NB_ERR_NOMEM;

	if (!row->turn_id_missing && row->turn_id.ptr) {
		value.turn_id = orm_text_copy(row->turn_id);
		if (!value.turn_id)
			return NB_ERR_NOMEM;
	}

	if (!row->content_missing && row->content.ptr) {
		value.content = orm_text_copy(row->content);
		if (!value.content)
			return NB_ERR_NOMEM;
	}

	if (!row->payload_json_missing && row->payload_json.ptr) {
		value.payload_json = orm_text_copy(row->payload_json);
		if (!value.payload_json)
			return NB_ERR_NOMEM;
	}

	if (!row->tool_call_id_missing && row->tool_call_id.ptr) {
		value.tool_call_id = orm_text_copy(row->tool_call_id);
		if (!value.tool_call_id)
			return NB_ERR_NOMEM;
	}

	if (!row->provider_call_id_missing && row->provider_call_id.ptr) {
		value.provider_call_id = orm_text_copy(row->provider_call_id);
		if (!value.provider_call_id)
			return NB_ERR_NOMEM;
	}

	if (!row->idempotency_key_missing && row->idempotency_key.ptr) {
		value.idempotency_key = orm_text_copy(row->idempotency_key);
		if (!value.idempotency_key)
			return NB_ERR_NOMEM;
	}

	*out = value;
	memset(&value, 0, sizeof(value));
	return 0;
}
