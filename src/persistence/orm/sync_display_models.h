#ifndef MORPH_ORM_SYNC_DISPLAY_MODELS_H
#define MORPH_ORM_SYNC_DISPLAY_MODELS_H

#include "persistence/orm/internal.h"

struct sync_display_row {
	int64_t session_id;
	bool missing_session_id;
	nb_str turn_id;
	bool missing_turn_id;
	int64_t core_message_id;
	bool missing_core_message_id;
	int64_t seq;
	bool missing_seq;
	nb_str type;
	bool missing_type;
	nb_str content;
	bool missing_content;
	nb_str attachments_json;
	bool missing_attachments_json;
	nb_str structured_data;
	bool missing_structured_data;
	nb_str agent_ui_ir;
	bool missing_agent_ui_ir;
	int64_t hitl_verdict;
	bool missing_hitl_verdict;
	int64_t created_at;
	bool missing_created_at;
	int64_t updated_at;
	bool missing_updated_at;
};
static const struct norm_field sync_display_fields[] = {
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct sync_display_row, session_id),
		.size = sizeof(((struct sync_display_row *)0)->session_id),
		.null_offset = offsetof(struct sync_display_row, missing_session_id),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("turn_id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct sync_display_row, turn_id),
		.size = sizeof(((struct sync_display_row *)0)->turn_id),
		.null_offset = offsetof(struct sync_display_row, missing_turn_id),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("core_message_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct sync_display_row, core_message_id),
		.size = sizeof(((struct sync_display_row *)0)->core_message_id),
		.null_offset = offsetof(struct sync_display_row, missing_core_message_id),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("seq"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct sync_display_row, seq),
		.size = sizeof(((struct sync_display_row *)0)->seq),
		.null_offset = offsetof(struct sync_display_row, missing_seq),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("type"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct sync_display_row, type),
		.size = sizeof(((struct sync_display_row *)0)->type),
		.null_offset = offsetof(struct sync_display_row, missing_type),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("content"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct sync_display_row, content),
		.size = sizeof(((struct sync_display_row *)0)->content),
		.null_offset = offsetof(struct sync_display_row, missing_content),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("attachments_json"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct sync_display_row, attachments_json),
		.size = sizeof(((struct sync_display_row *)0)->attachments_json),
		.null_offset = offsetof(struct sync_display_row, missing_attachments_json),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("structured_data"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct sync_display_row, structured_data),
		.size = sizeof(((struct sync_display_row *)0)->structured_data),
		.null_offset = offsetof(struct sync_display_row, missing_structured_data),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("agent_ui_ir"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct sync_display_row, agent_ui_ir),
		.size = sizeof(((struct sync_display_row *)0)->agent_ui_ir),
		.null_offset = offsetof(struct sync_display_row, missing_agent_ui_ir),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("hitl_verdict"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct sync_display_row, hitl_verdict),
		.size = sizeof(((struct sync_display_row *)0)->hitl_verdict),
		.null_offset = offsetof(struct sync_display_row, missing_hitl_verdict),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct sync_display_row, created_at),
		.size = sizeof(((struct sync_display_row *)0)->created_at),
		.null_offset = offsetof(struct sync_display_row, missing_created_at),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("updated_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct sync_display_row, updated_at),
		.size = sizeof(((struct sync_display_row *)0)->updated_at),
		.null_offset = offsetof(struct sync_display_row, missing_updated_at),
		.flags = NORM_FIELD_NULLABLE,
	},
};
static const struct norm_model sync_display_model = {

	.table = NB_STR_LIT("sync_ui_messages"),
	.size = sizeof(struct sync_display_row),
	.alignment = _Alignof(struct sync_display_row),
	.fields = sync_display_fields,
	.field_count = sizeof(sync_display_fields) / sizeof(sync_display_fields[0]),
};

#endif
