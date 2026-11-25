#include "schema_models.h"
#include "records.h"
#include <string.h>

static const struct norm_model tables[SCHEMA_TABLE_COUNT];

struct schema_schema_migrations_row {
	int64_t version;
	nb_str name;
	int64_t applied_at;
};

static const struct norm_field schema_migrations_fields[] = {
	{
		.column = NB_STR_LIT("version"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_schema_migrations_row, version),
		.size = sizeof(((struct schema_schema_migrations_row *)0)->version),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("name"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_schema_migrations_row, name),
		.size = sizeof(((struct schema_schema_migrations_row *)0)->name),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("applied_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_schema_migrations_row, applied_at),
		.size = sizeof(((struct schema_schema_migrations_row *)0)->applied_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

struct schema_sessions_row {
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

static const struct norm_field sessions_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_sessions_row, id),
		.size = sizeof(((struct schema_sessions_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("display_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_sessions_row, display_id),
		.size = sizeof(((struct schema_sessions_row *)0)->display_id),
		.null_offset = offsetof(struct schema_sessions_row, display_id_missing),
		.flags = NORM_FIELD_NULLABLE | NORM_FIELD_UNIQUE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("name"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_sessions_row, name),
		.size = sizeof(((struct schema_sessions_row *)0)->name),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_UNIQUE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("model"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_sessions_row, model),
		.size = sizeof(((struct schema_sessions_row *)0)->model),
		.null_offset = offsetof(struct schema_sessions_row, model_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("workdir"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_sessions_row, workdir),
		.size = sizeof(((struct schema_sessions_row *)0)->workdir),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
		.default_sql = NB_STR_LIT("''"),
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_sessions_row, created_at),
		.size = sizeof(((struct schema_sessions_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("updated_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_sessions_row, updated_at),
		.size = sizeof(((struct schema_sessions_row *)0)->updated_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("token_used"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_sessions_row, token_used),
		.size = sizeof(((struct schema_sessions_row *)0)->token_used),
		.null_offset = offsetof(struct schema_sessions_row, token_used_missing),
		.flags = NORM_FIELD_NULLABLE,
		.default_sql = NB_STR_LIT("0"),
	},
};

static const size_t idx_sessions_display_id_fields[] = {1};
static const enum norm_sort_direction idx_sessions_display_id_directions[] = {NORM_ASC};
static const struct norm_index sessions_indexes[] = {
	{.name = NB_STR_LIT("idx_sessions_display_id"),
	 .fields = idx_sessions_display_id_fields,
	 .field_count = NB_ARRAY_SIZE(idx_sessions_display_id_fields),
	 .unique = true,
	 .directions = idx_sessions_display_id_directions},
};

struct schema_session_skill_activations_row {
	int64_t session_id;
	nb_str skill_name;
};

static const struct norm_field session_skill_activations_fields[] = {
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_session_skill_activations_row, session_id),
		.size = sizeof(((struct schema_session_skill_activations_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("skill_name"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_session_skill_activations_row, skill_name),
		.size = sizeof(((struct schema_session_skill_activations_row *)0)->skill_name),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
		.max_length = SIZE_MAX,
	},
};

static const size_t session_skill_activations_fk_0_fields[] = {0};
static const size_t session_skill_activations_fk_0_references[] = {0};
static const struct norm_foreign_key session_skill_activations_foreign_keys[] = {
	{.name = NB_STR_LIT("session_skill_activations_fk_0"),
	 .fields = session_skill_activations_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(session_skill_activations_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = session_skill_activations_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

struct schema_messages_row {
	int64_t id;
	int64_t session_id;
	nb_str role;
	nb_str content;
	nb_str turn_id;
	bool turn_id_missing;
	int64_t token_count;
	int64_t compressed;
	bool compressed_missing;
	int64_t created_at;
};

static const struct norm_field messages_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_messages_row, id),
		.size = sizeof(((struct schema_messages_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_messages_row, session_id),
		.size = sizeof(((struct schema_messages_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("role"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_messages_row, role),
		.size = sizeof(((struct schema_messages_row *)0)->role),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("content"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_messages_row, content),
		.size = sizeof(((struct schema_messages_row *)0)->content),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("turn_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_messages_row, turn_id),
		.size = sizeof(((struct schema_messages_row *)0)->turn_id),
		.null_offset = offsetof(struct schema_messages_row, turn_id_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("token_count"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_messages_row, token_count),
		.size = sizeof(((struct schema_messages_row *)0)->token_count),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("compressed"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_messages_row, compressed),
		.size = sizeof(((struct schema_messages_row *)0)->compressed),
		.null_offset = offsetof(struct schema_messages_row, compressed_missing),
		.flags = NORM_FIELD_NULLABLE,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_messages_row, created_at),
		.size = sizeof(((struct schema_messages_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

static const size_t messages_fk_0_fields[] = {1};
static const size_t messages_fk_0_references[] = {0};
static const struct norm_foreign_key messages_foreign_keys[] = {
	{.name = NB_STR_LIT("messages_fk_0"),
	 .fields = messages_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(messages_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = messages_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

static const size_t idx_messages_session_order_fields[] = {1, 7, 0};
static const enum norm_sort_direction idx_messages_session_order_directions[] = {NORM_ASC, NORM_ASC,
										 NORM_ASC};
static const size_t idx_messages_session_fields[] = {1, 7};
static const enum norm_sort_direction idx_messages_session_directions[] = {NORM_ASC, NORM_ASC};
static const struct norm_index messages_indexes[] = {
	{.name = NB_STR_LIT("idx_messages_session_order"),
	 .fields = idx_messages_session_order_fields,
	 .field_count = NB_ARRAY_SIZE(idx_messages_session_order_fields),
	 .unique = false,
	 .directions = idx_messages_session_order_directions},
	{.name = NB_STR_LIT("idx_messages_session"),
	 .fields = idx_messages_session_fields,
	 .field_count = NB_ARRAY_SIZE(idx_messages_session_fields),
	 .unique = false,
	 .directions = idx_messages_session_directions},
};

struct schema_message_attachments_row {
	int64_t id;
	int64_t message_id;
	nb_str kind;
	nb_str path;
	nb_str sha256;
	bool sha256_missing;
};

static const struct norm_field message_attachments_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_message_attachments_row, id),
		.size = sizeof(((struct schema_message_attachments_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("message_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_message_attachments_row, message_id),
		.size = sizeof(((struct schema_message_attachments_row *)0)->message_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("kind"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_message_attachments_row, kind),
		.size = sizeof(((struct schema_message_attachments_row *)0)->kind),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("path"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_message_attachments_row, path),
		.size = sizeof(((struct schema_message_attachments_row *)0)->path),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("sha256"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_message_attachments_row, sha256),
		.size = sizeof(((struct schema_message_attachments_row *)0)->sha256),
		.null_offset = offsetof(struct schema_message_attachments_row, sha256_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
};

static const size_t message_attachments_fk_0_fields[] = {1};
static const size_t message_attachments_fk_0_references[] = {0};
static const struct norm_foreign_key message_attachments_foreign_keys[] = {
	{.name = NB_STR_LIT("message_attachments_fk_0"),
	 .fields = message_attachments_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(message_attachments_fk_0_fields),
	 .references = &tables[SCHEMA_MESSAGES],
	 .reference_fields = message_attachments_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

struct schema_react_traces_row {
	int64_t id;
	int64_t session_id;
	int64_t round_no;
	nb_str steps_json;
	int64_t aborted;
	bool aborted_missing;
	int64_t created_at;
};

static const struct norm_field react_traces_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_react_traces_row, id),
		.size = sizeof(((struct schema_react_traces_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_react_traces_row, session_id),
		.size = sizeof(((struct schema_react_traces_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("round_no"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_react_traces_row, round_no),
		.size = sizeof(((struct schema_react_traces_row *)0)->round_no),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("steps_json"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_react_traces_row, steps_json),
		.size = sizeof(((struct schema_react_traces_row *)0)->steps_json),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("aborted"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_react_traces_row, aborted),
		.size = sizeof(((struct schema_react_traces_row *)0)->aborted),
		.null_offset = offsetof(struct schema_react_traces_row, aborted_missing),
		.flags = NORM_FIELD_NULLABLE,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_react_traces_row, created_at),
		.size = sizeof(((struct schema_react_traces_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

static const size_t react_traces_fk_0_fields[] = {1};
static const size_t react_traces_fk_0_references[] = {0};
static const struct norm_foreign_key react_traces_foreign_keys[] = {
	{.name = NB_STR_LIT("react_traces_fk_0"),
	 .fields = react_traces_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(react_traces_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = react_traces_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

static const size_t idx_traces_session_fields[] = {1, 2};
static const enum norm_sort_direction idx_traces_session_directions[] = {NORM_ASC, NORM_ASC};
static const struct norm_index react_traces_indexes[] = {
	{.name = NB_STR_LIT("idx_traces_session"),
	 .fields = idx_traces_session_fields,
	 .field_count = NB_ARRAY_SIZE(idx_traces_session_fields),
	 .unique = false,
	 .directions = idx_traces_session_directions},
};

struct schema_exts_row {
	int64_t id;
	nb_str name;
	nb_str version;
	bool version_missing;
	nb_str path;
	nb_str type;
	int64_t permissions;
	bool permissions_missing;
	int64_t enabled;
	bool enabled_missing;
	int64_t installed_at;
};

static const struct norm_field exts_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_exts_row, id),
		.size = sizeof(((struct schema_exts_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("name"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_exts_row, name),
		.size = sizeof(((struct schema_exts_row *)0)->name),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_UNIQUE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("version"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_exts_row, version),
		.size = sizeof(((struct schema_exts_row *)0)->version),
		.null_offset = offsetof(struct schema_exts_row, version_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("path"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_exts_row, path),
		.size = sizeof(((struct schema_exts_row *)0)->path),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("type"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_exts_row, type),
		.size = sizeof(((struct schema_exts_row *)0)->type),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("permissions"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_exts_row, permissions),
		.size = sizeof(((struct schema_exts_row *)0)->permissions),
		.null_offset = offsetof(struct schema_exts_row, permissions_missing),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("enabled"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_exts_row, enabled),
		.size = sizeof(((struct schema_exts_row *)0)->enabled),
		.null_offset = offsetof(struct schema_exts_row, enabled_missing),
		.flags = NORM_FIELD_NULLABLE,
		.default_sql = NB_STR_LIT("1"),
	},
	{
		.column = NB_STR_LIT("installed_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_exts_row, installed_at),
		.size = sizeof(((struct schema_exts_row *)0)->installed_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

struct schema_outputs_row {
	int64_t id;
	int64_t session_id;
	bool session_id_missing;
	nb_str kind;
	nb_str path;
	nb_str prompt;
	bool prompt_missing;
	nb_str model;
	bool model_missing;
	nb_str request_prompt;
	bool request_prompt_missing;
	nb_str provider;
	bool provider_missing;
	nb_str tool_name;
	bool tool_name_missing;
	nb_str idempotency_key;
	bool idempotency_key_missing;
	nb_str tool_call_id;
	bool tool_call_id_missing;
	nb_str turn_id;
	bool turn_id_missing;
	nb_str recipe_json;
	bool recipe_json_missing;
	nb_str mime;
	bool mime_missing;
	int64_t width;
	int64_t height;
	int64_t duration_seconds;
	int64_t size_bytes;
	int64_t created_at;
};

static const struct norm_field outputs_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_outputs_row, id),
		.size = sizeof(((struct schema_outputs_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_outputs_row, session_id),
		.size = sizeof(((struct schema_outputs_row *)0)->session_id),
		.null_offset = offsetof(struct schema_outputs_row, session_id_missing),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("kind"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_outputs_row, kind),
		.size = sizeof(((struct schema_outputs_row *)0)->kind),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("path"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_outputs_row, path),
		.size = sizeof(((struct schema_outputs_row *)0)->path),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("prompt"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_outputs_row, prompt),
		.size = sizeof(((struct schema_outputs_row *)0)->prompt),
		.null_offset = offsetof(struct schema_outputs_row, prompt_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("model"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_outputs_row, model),
		.size = sizeof(((struct schema_outputs_row *)0)->model),
		.null_offset = offsetof(struct schema_outputs_row, model_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("request_prompt"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_outputs_row, request_prompt),
		.size = sizeof(((struct schema_outputs_row *)0)->request_prompt),
		.null_offset = offsetof(struct schema_outputs_row, request_prompt_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("provider"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_outputs_row, provider),
		.size = sizeof(((struct schema_outputs_row *)0)->provider),
		.null_offset = offsetof(struct schema_outputs_row, provider_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("tool_name"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_outputs_row, tool_name),
		.size = sizeof(((struct schema_outputs_row *)0)->tool_name),
		.null_offset = offsetof(struct schema_outputs_row, tool_name_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("idempotency_key"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_outputs_row, idempotency_key),
		.size = sizeof(((struct schema_outputs_row *)0)->idempotency_key),
		.null_offset = offsetof(struct schema_outputs_row, idempotency_key_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("tool_call_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_outputs_row, tool_call_id),
		.size = sizeof(((struct schema_outputs_row *)0)->tool_call_id),
		.null_offset = offsetof(struct schema_outputs_row, tool_call_id_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("turn_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_outputs_row, turn_id),
		.size = sizeof(((struct schema_outputs_row *)0)->turn_id),
		.null_offset = offsetof(struct schema_outputs_row, turn_id_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("recipe_json"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_outputs_row, recipe_json),
		.size = sizeof(((struct schema_outputs_row *)0)->recipe_json),
		.null_offset = offsetof(struct schema_outputs_row, recipe_json_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("mime"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_outputs_row, mime),
		.size = sizeof(((struct schema_outputs_row *)0)->mime),
		.null_offset = offsetof(struct schema_outputs_row, mime_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("width"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_outputs_row, width),
		.size = sizeof(((struct schema_outputs_row *)0)->width),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("height"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_outputs_row, height),
		.size = sizeof(((struct schema_outputs_row *)0)->height),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("duration_seconds"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_outputs_row, duration_seconds),
		.size = sizeof(((struct schema_outputs_row *)0)->duration_seconds),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("size_bytes"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_outputs_row, size_bytes),
		.size = sizeof(((struct schema_outputs_row *)0)->size_bytes),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_outputs_row, created_at),
		.size = sizeof(((struct schema_outputs_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

static const size_t outputs_fk_0_fields[] = {1};
static const size_t outputs_fk_0_references[] = {0};
static const struct norm_foreign_key outputs_foreign_keys[] = {
	{.name = NB_STR_LIT("outputs_fk_0"),
	 .fields = outputs_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(outputs_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = outputs_fk_0_references,
	 .on_delete = NORM_FK_SET_NULL,
	 .on_update = NORM_FK_NO_ACTION},
};

static const size_t idx_outputs_path_fields[] = {3, 0};
static const enum norm_sort_direction idx_outputs_path_directions[] = {NORM_ASC, NORM_DESC};
static const size_t idx_outputs_session_fields[] = {1, 18};
static const enum norm_sort_direction idx_outputs_session_directions[] = {NORM_ASC, NORM_ASC};
static const struct norm_index outputs_indexes[] = {
	{.name = NB_STR_LIT("idx_outputs_path"),
	 .fields = idx_outputs_path_fields,
	 .field_count = NB_ARRAY_SIZE(idx_outputs_path_fields),
	 .unique = false,
	 .directions = idx_outputs_path_directions},
	{.name = NB_STR_LIT("idx_outputs_session"),
	 .fields = idx_outputs_session_fields,
	 .field_count = NB_ARRAY_SIZE(idx_outputs_session_fields),
	 .unique = false,
	 .directions = idx_outputs_session_directions},
};

struct schema_memory_profiles_row {
	int64_t session_id;
	nb_str profile_text;
	int64_t updated_at;
};

static const struct norm_field memory_profiles_fields[] = {
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_profiles_row, session_id),
		.size = sizeof(((struct schema_memory_profiles_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("profile_text"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_profiles_row, profile_text),
		.size = sizeof(((struct schema_memory_profiles_row *)0)->profile_text),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
		.default_sql = NB_STR_LIT("''"),
	},
	{
		.column = NB_STR_LIT("updated_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_profiles_row, updated_at),
		.size = sizeof(((struct schema_memory_profiles_row *)0)->updated_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

static const size_t memory_profiles_fk_0_fields[] = {0};
static const size_t memory_profiles_fk_0_references[] = {0};
static const struct norm_foreign_key memory_profiles_foreign_keys[] = {
	{.name = NB_STR_LIT("memory_profiles_fk_0"),
	 .fields = memory_profiles_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(memory_profiles_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = memory_profiles_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

struct schema_memory_facts_row {
	int64_t id;
	int64_t session_id;
	nb_str key_name;
	nb_str value_text;
	nb_str source_text;
	bool source_text_missing;
	double confidence;
	bool confidence_missing;
	nb_str category;
	bool category_missing;
	double importance;
	bool importance_missing;
	int64_t is_current;
	int64_t valid_from;
	int64_t valid_to;
	bool valid_to_missing;
	int64_t superseded_by;
	bool superseded_by_missing;
	int64_t created_at;
	int64_t updated_at;
};

static const struct norm_field memory_facts_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_facts_row, id),
		.size = sizeof(((struct schema_memory_facts_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_facts_row, session_id),
		.size = sizeof(((struct schema_memory_facts_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("key_name"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_facts_row, key_name),
		.size = sizeof(((struct schema_memory_facts_row *)0)->key_name),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("value_text"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_facts_row, value_text),
		.size = sizeof(((struct schema_memory_facts_row *)0)->value_text),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("source_text"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_facts_row, source_text),
		.size = sizeof(((struct schema_memory_facts_row *)0)->source_text),
		.null_offset = offsetof(struct schema_memory_facts_row, source_text_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("confidence"),
		.type = NORM_TYPE_F64,
		.offset = offsetof(struct schema_memory_facts_row, confidence),
		.size = sizeof(((struct schema_memory_facts_row *)0)->confidence),
		.null_offset = offsetof(struct schema_memory_facts_row, confidence_missing),
		.flags = NORM_FIELD_NULLABLE,
		.default_sql = NB_STR_LIT("1.0"),
	},
	{
		.column = NB_STR_LIT("category"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_facts_row, category),
		.size = sizeof(((struct schema_memory_facts_row *)0)->category),
		.null_offset = offsetof(struct schema_memory_facts_row, category_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
		.default_sql = NB_STR_LIT("'general'"),
	},
	{
		.column = NB_STR_LIT("importance"),
		.type = NORM_TYPE_F64,
		.offset = offsetof(struct schema_memory_facts_row, importance),
		.size = sizeof(((struct schema_memory_facts_row *)0)->importance),
		.null_offset = offsetof(struct schema_memory_facts_row, importance_missing),
		.flags = NORM_FIELD_NULLABLE,
		.default_sql = NB_STR_LIT("0.5"),
	},
	{
		.column = NB_STR_LIT("is_current"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_facts_row, is_current),
		.size = sizeof(((struct schema_memory_facts_row *)0)->is_current),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("1"),
	},
	{
		.column = NB_STR_LIT("valid_from"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_facts_row, valid_from),
		.size = sizeof(((struct schema_memory_facts_row *)0)->valid_from),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("valid_to"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_facts_row, valid_to),
		.size = sizeof(((struct schema_memory_facts_row *)0)->valid_to),
		.null_offset = offsetof(struct schema_memory_facts_row, valid_to_missing),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("superseded_by"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_facts_row, superseded_by),
		.size = sizeof(((struct schema_memory_facts_row *)0)->superseded_by),
		.null_offset = offsetof(struct schema_memory_facts_row, superseded_by_missing),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_facts_row, created_at),
		.size = sizeof(((struct schema_memory_facts_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("updated_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_facts_row, updated_at),
		.size = sizeof(((struct schema_memory_facts_row *)0)->updated_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

static const size_t memory_facts_fk_0_fields[] = {1};
static const size_t memory_facts_fk_0_references[] = {0};
static const struct norm_foreign_key memory_facts_foreign_keys[] = {
	{.name = NB_STR_LIT("memory_facts_fk_0"),
	 .fields = memory_facts_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(memory_facts_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = memory_facts_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

static const size_t idx_memory_facts_session_key_fields[] = {1, 2, 8, 13};
static const enum norm_sort_direction idx_memory_facts_session_key_directions[] = {
	NORM_ASC, NORM_ASC, NORM_ASC, NORM_ASC};
static const struct norm_index memory_facts_indexes[] = {
	{.name = NB_STR_LIT("idx_memory_facts_session_key"),
	 .fields = idx_memory_facts_session_key_fields,
	 .field_count = NB_ARRAY_SIZE(idx_memory_facts_session_key_fields),
	 .unique = false,
	 .directions = idx_memory_facts_session_key_directions},
};

struct schema_memory_episodes_row {
	int64_t id;
	int64_t session_id;
	nb_str task_type;
	bool task_type_missing;
	nb_str summary_text;
	nb_str outcome_text;
	bool outcome_text_missing;
	int64_t success;
	nb_str entities;
	bool entities_missing;
	nb_str key_decisions;
	bool key_decisions_missing;
	nb_str artifacts;
	bool artifacts_missing;
	nb_str tools_used;
	bool tools_used_missing;
	double importance;
	bool importance_missing;
	int64_t created_at;
	int64_t job_id;
	bool job_id_missing;
};

static const struct norm_field memory_episodes_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_episodes_row, id),
		.size = sizeof(((struct schema_memory_episodes_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_episodes_row, session_id),
		.size = sizeof(((struct schema_memory_episodes_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("task_type"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_episodes_row, task_type),
		.size = sizeof(((struct schema_memory_episodes_row *)0)->task_type),
		.null_offset = offsetof(struct schema_memory_episodes_row, task_type_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("summary_text"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_episodes_row, summary_text),
		.size = sizeof(((struct schema_memory_episodes_row *)0)->summary_text),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("outcome_text"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_episodes_row, outcome_text),
		.size = sizeof(((struct schema_memory_episodes_row *)0)->outcome_text),
		.null_offset = offsetof(struct schema_memory_episodes_row, outcome_text_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("success"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_episodes_row, success),
		.size = sizeof(((struct schema_memory_episodes_row *)0)->success),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("1"),
	},
	{
		.column = NB_STR_LIT("entities"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_episodes_row, entities),
		.size = sizeof(((struct schema_memory_episodes_row *)0)->entities),
		.null_offset = offsetof(struct schema_memory_episodes_row, entities_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("key_decisions"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_episodes_row, key_decisions),
		.size = sizeof(((struct schema_memory_episodes_row *)0)->key_decisions),
		.null_offset = offsetof(struct schema_memory_episodes_row, key_decisions_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("artifacts"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_episodes_row, artifacts),
		.size = sizeof(((struct schema_memory_episodes_row *)0)->artifacts),
		.null_offset = offsetof(struct schema_memory_episodes_row, artifacts_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("tools_used"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_episodes_row, tools_used),
		.size = sizeof(((struct schema_memory_episodes_row *)0)->tools_used),
		.null_offset = offsetof(struct schema_memory_episodes_row, tools_used_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("importance"),
		.type = NORM_TYPE_F64,
		.offset = offsetof(struct schema_memory_episodes_row, importance),
		.size = sizeof(((struct schema_memory_episodes_row *)0)->importance),
		.null_offset = offsetof(struct schema_memory_episodes_row, importance_missing),
		.flags = NORM_FIELD_NULLABLE,
		.default_sql = NB_STR_LIT("0.5"),
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_episodes_row, created_at),
		.size = sizeof(((struct schema_memory_episodes_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("job_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_episodes_row, job_id),
		.size = sizeof(((struct schema_memory_episodes_row *)0)->job_id),
		.null_offset = offsetof(struct schema_memory_episodes_row, job_id_missing),
		.flags = NORM_FIELD_NULLABLE,
	},
};

static const size_t memory_episodes_fk_0_fields[] = {1};
static const size_t memory_episodes_fk_0_references[] = {0};
static const struct norm_foreign_key memory_episodes_foreign_keys[] = {
	{.name = NB_STR_LIT("memory_episodes_fk_0"),
	 .fields = memory_episodes_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(memory_episodes_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = memory_episodes_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

static const size_t idx_memory_episode_job_fields[] = {12};
static const enum norm_sort_direction idx_memory_episode_job_directions[] = {NORM_ASC};
static const struct norm_expr idx_memory_episode_job_predicate = {
	.kind = NORM_EXPR_IS_NOT_NULL,
	.as.null_column = {.model = &tables[SCHEMA_MEMORY_EPISODES], .index = 12}};
static const size_t idx_memory_episodes_session_fields[] = {1, 11};
static const enum norm_sort_direction idx_memory_episodes_session_directions[] = {NORM_ASC,
										  NORM_ASC};
static const struct norm_index memory_episodes_indexes[] = {
	{.name = NB_STR_LIT("idx_memory_episode_job"),
	 .fields = idx_memory_episode_job_fields,
	 .field_count = NB_ARRAY_SIZE(idx_memory_episode_job_fields),
	 .unique = true,
	 .directions = idx_memory_episode_job_directions,
	 .predicate = &idx_memory_episode_job_predicate},
	{.name = NB_STR_LIT("idx_memory_episodes_session"),
	 .fields = idx_memory_episodes_session_fields,
	 .field_count = NB_ARRAY_SIZE(idx_memory_episodes_session_fields),
	 .unique = false,
	 .directions = idx_memory_episodes_session_directions},
};

struct schema_memory_procedures_row {
	int64_t id;
	int64_t session_id;
	nb_str rule_text;
	nb_str trigger_text;
	bool trigger_text_missing;
	int64_t evidence_count;
	int64_t updated_at;
};

static const struct norm_field memory_procedures_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_procedures_row, id),
		.size = sizeof(((struct schema_memory_procedures_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_procedures_row, session_id),
		.size = sizeof(((struct schema_memory_procedures_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("rule_text"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_procedures_row, rule_text),
		.size = sizeof(((struct schema_memory_procedures_row *)0)->rule_text),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("trigger_text"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_procedures_row, trigger_text),
		.size = sizeof(((struct schema_memory_procedures_row *)0)->trigger_text),
		.null_offset = offsetof(struct schema_memory_procedures_row, trigger_text_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("evidence_count"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_procedures_row, evidence_count),
		.size = sizeof(((struct schema_memory_procedures_row *)0)->evidence_count),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("1"),
	},
	{
		.column = NB_STR_LIT("updated_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_procedures_row, updated_at),
		.size = sizeof(((struct schema_memory_procedures_row *)0)->updated_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

static const size_t memory_procedures_fk_0_fields[] = {1};
static const size_t memory_procedures_fk_0_references[] = {0};
static const struct norm_foreign_key memory_procedures_foreign_keys[] = {
	{.name = NB_STR_LIT("memory_procedures_fk_0"),
	 .fields = memory_procedures_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(memory_procedures_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = memory_procedures_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

static const size_t idx_memory_procedures_session_fields[] = {1, 5};
static const enum norm_sort_direction idx_memory_procedures_session_directions[] = {NORM_ASC,
										    NORM_ASC};
static const size_t sqlite_autoindex_memory_procedures_1_fields[] = {1, 2};
static const enum norm_sort_direction sqlite_autoindex_memory_procedures_1_directions[] = {
	NORM_ASC, NORM_ASC};
static const struct norm_index memory_procedures_indexes[] = {
	{.name = NB_STR_LIT("idx_memory_procedures_session"),
	 .fields = idx_memory_procedures_session_fields,
	 .field_count = NB_ARRAY_SIZE(idx_memory_procedures_session_fields),
	 .unique = false,
	 .directions = idx_memory_procedures_session_directions},
	{.name = NB_STR_LIT("uq_memory_procedures_session_id_rule_text"),
	 .fields = sqlite_autoindex_memory_procedures_1_fields,
	 .field_count = NB_ARRAY_SIZE(sqlite_autoindex_memory_procedures_1_fields),
	 .unique = true,
	 .directions = sqlite_autoindex_memory_procedures_1_directions},
};

struct schema_memory_scopes_row {
	int64_t session_id;
	nb_str owner;
	nb_str project;
	int64_t migrated;
	int64_t generation;
	int64_t content_revision;
};

static const struct norm_field memory_scopes_fields[] = {
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_scopes_row, session_id),
		.size = sizeof(((struct schema_memory_scopes_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("owner"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_scopes_row, owner),
		.size = sizeof(((struct schema_memory_scopes_row *)0)->owner),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("project"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_scopes_row, project),
		.size = sizeof(((struct schema_memory_scopes_row *)0)->project),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
		.default_sql = NB_STR_LIT("''"),
	},
	{
		.column = NB_STR_LIT("migrated"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_scopes_row, migrated),
		.size = sizeof(((struct schema_memory_scopes_row *)0)->migrated),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("generation"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_scopes_row, generation),
		.size = sizeof(((struct schema_memory_scopes_row *)0)->generation),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("content_revision"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_scopes_row, content_revision),
		.size = sizeof(((struct schema_memory_scopes_row *)0)->content_revision),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
};

static const size_t memory_scopes_fk_0_fields[] = {0};
static const size_t memory_scopes_fk_0_references[] = {0};
static const struct norm_foreign_key memory_scopes_foreign_keys[] = {
	{.name = NB_STR_LIT("memory_scopes_fk_0"),
	 .fields = memory_scopes_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(memory_scopes_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = memory_scopes_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

static const size_t idx_memory_scopes_owner_project_fields[] = {1, 2, 0};
static const enum norm_sort_direction idx_memory_scopes_owner_project_directions[] = {
	NORM_ASC, NORM_ASC, NORM_ASC};
static const struct norm_index memory_scopes_indexes[] = {
	{.name = NB_STR_LIT("idx_memory_scopes_owner_project"),
	 .fields = idx_memory_scopes_owner_project_fields,
	 .field_count = NB_ARRAY_SIZE(idx_memory_scopes_owner_project_fields),
	 .unique = false,
	 .directions = idx_memory_scopes_owner_project_directions},
};

struct schema_memory_preferences_row {
	int64_t id;
	nb_str owner;
	nb_str scope;
	nb_str target;
	nb_str key;
	nb_str value;
	bool value_missing;
	nb_str source;
	nb_str origin;
	int64_t source_order;
	int64_t session_id;
	nb_str event_token;
	int64_t created_at;
};

static const struct norm_field memory_preferences_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_preferences_row, id),
		.size = sizeof(((struct schema_memory_preferences_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("owner"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_preferences_row, owner),
		.size = sizeof(((struct schema_memory_preferences_row *)0)->owner),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("scope"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_preferences_row, scope),
		.size = sizeof(((struct schema_memory_preferences_row *)0)->scope),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("target"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_preferences_row, target),
		.size = sizeof(((struct schema_memory_preferences_row *)0)->target),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("key"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_preferences_row, key),
		.size = sizeof(((struct schema_memory_preferences_row *)0)->key),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("value"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_preferences_row, value),
		.size = sizeof(((struct schema_memory_preferences_row *)0)->value),
		.null_offset = offsetof(struct schema_memory_preferences_row, value_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("source"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_preferences_row, source),
		.size = sizeof(((struct schema_memory_preferences_row *)0)->source),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("origin"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_preferences_row, origin),
		.size = sizeof(((struct schema_memory_preferences_row *)0)->origin),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
		.default_sql = NB_STR_LIT("'explicit'"),
	},
	{
		.column = NB_STR_LIT("source_order"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_preferences_row, source_order),
		.size = sizeof(((struct schema_memory_preferences_row *)0)->source_order),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_preferences_row, session_id),
		.size = sizeof(((struct schema_memory_preferences_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("event_token"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_preferences_row, event_token),
		.size = sizeof(((struct schema_memory_preferences_row *)0)->event_token),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_preferences_row, created_at),
		.size = sizeof(((struct schema_memory_preferences_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_kind = NORM_DEFAULT_CURRENT_EPOCH,
	},
};

static const size_t idx_preferences_resolve_fields[] = {1, 2, 3, 4, 0};
static const enum norm_sort_direction idx_preferences_resolve_directions[] = {
	NORM_ASC, NORM_ASC, NORM_ASC, NORM_ASC, NORM_DESC};
static const size_t sqlite_autoindex_memory_preferences_1_fields[] = {1, 2, 3, 4, 9, 10};
static const enum norm_sort_direction sqlite_autoindex_memory_preferences_1_directions[] = {
	NORM_ASC, NORM_ASC, NORM_ASC, NORM_ASC, NORM_ASC, NORM_ASC};
static const struct norm_index memory_preferences_indexes[] = {
	{.name = NB_STR_LIT("idx_preferences_resolve"),
	 .fields = idx_preferences_resolve_fields,
	 .field_count = NB_ARRAY_SIZE(idx_preferences_resolve_fields),
	 .unique = false,
	 .directions = idx_preferences_resolve_directions},
	{.name = NB_STR_LIT("uq_memory_preferences_identity"),
	 .fields = sqlite_autoindex_memory_preferences_1_fields,
	 .field_count = NB_ARRAY_SIZE(sqlite_autoindex_memory_preferences_1_fields),
	 .unique = true,
	 .directions = sqlite_autoindex_memory_preferences_1_directions},
};

static const struct norm_value memory_preferences_check_0_values[] = {
	{.type = NORM_TYPE_TEXT, .as.text = NB_STR_LIT("personal")},
	{.type = NORM_TYPE_TEXT, .as.text = NB_STR_LIT("project")},
	{.type = NORM_TYPE_TEXT, .as.text = NB_STR_LIT("session")}};
static const struct norm_expr memory_preferences_check_0_predicate = {
	.kind = NORM_EXPR_IN,
	.as.in = {.column = {.model = &tables[SCHEMA_MEMORY_PREFERENCES], .index = 2},
		  .values = memory_preferences_check_0_values,
		  .count = NB_ARRAY_SIZE(memory_preferences_check_0_values)}};
static const struct norm_check memory_preferences_checks[] = {
	{.name = NB_STR_LIT("memory_preferences_check_0"),
	 .predicate = &memory_preferences_check_0_predicate},
};

struct schema_memory_candidates_row {
	int64_t id;
	int64_t session_id;
	nb_str kind;
	nb_str key;
	nb_str value;
	nb_str source;
	int64_t created_at;
};

static const struct norm_field memory_candidates_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_candidates_row, id),
		.size = sizeof(((struct schema_memory_candidates_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_candidates_row, session_id),
		.size = sizeof(((struct schema_memory_candidates_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("kind"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_candidates_row, kind),
		.size = sizeof(((struct schema_memory_candidates_row *)0)->kind),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("key"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_candidates_row, key),
		.size = sizeof(((struct schema_memory_candidates_row *)0)->key),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("value"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_candidates_row, value),
		.size = sizeof(((struct schema_memory_candidates_row *)0)->value),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("source"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_candidates_row, source),
		.size = sizeof(((struct schema_memory_candidates_row *)0)->source),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_candidates_row, created_at),
		.size = sizeof(((struct schema_memory_candidates_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_kind = NORM_DEFAULT_CURRENT_EPOCH,
	},
};

static const size_t memory_candidates_fk_0_fields[] = {1};
static const size_t memory_candidates_fk_0_references[] = {0};
static const struct norm_foreign_key memory_candidates_foreign_keys[] = {
	{.name = NB_STR_LIT("memory_candidates_fk_0"),
	 .fields = memory_candidates_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(memory_candidates_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = memory_candidates_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

static const size_t idx_memory_candidates_lookup_fields[] = {1, 2, 3, 6, 0};
static const enum norm_sort_direction idx_memory_candidates_lookup_directions[] = {
	NORM_ASC, NORM_ASC, NORM_ASC, NORM_DESC, NORM_DESC};
static const size_t sqlite_autoindex_memory_candidates_1_fields[] = {1, 2, 3, 4, 5};
static const enum norm_sort_direction sqlite_autoindex_memory_candidates_1_directions[] = {
	NORM_ASC, NORM_ASC, NORM_ASC, NORM_ASC, NORM_ASC};
static const struct norm_index memory_candidates_indexes[] = {
	{.name = NB_STR_LIT("idx_memory_candidates_lookup"),
	 .fields = idx_memory_candidates_lookup_fields,
	 .field_count = NB_ARRAY_SIZE(idx_memory_candidates_lookup_fields),
	 .unique = false,
	 .directions = idx_memory_candidates_lookup_directions},
	{.name = NB_STR_LIT("uq_memory_candidates_session_id_kind_key_value_source"),
	 .fields = sqlite_autoindex_memory_candidates_1_fields,
	 .field_count = NB_ARRAY_SIZE(sqlite_autoindex_memory_candidates_1_fields),
	 .unique = true,
	 .directions = sqlite_autoindex_memory_candidates_1_directions},
};

struct schema_memory_jobs_row {
	int64_t id;
	int64_t session_id;
	int64_t generation;
	nb_str payload;
	nb_str state;
	int64_t attempts;
	int64_t worker_pid;
	int64_t error_code;
	int64_t updated_at;
	int64_t content_revision;
	nb_str stage;
	int64_t legacy_error_code;
	bool legacy_error_code_missing;
};

static const struct norm_field memory_jobs_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_jobs_row, id),
		.size = sizeof(((struct schema_memory_jobs_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_jobs_row, session_id),
		.size = sizeof(((struct schema_memory_jobs_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("generation"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_jobs_row, generation),
		.size = sizeof(((struct schema_memory_jobs_row *)0)->generation),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("payload"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_jobs_row, payload),
		.size = sizeof(((struct schema_memory_jobs_row *)0)->payload),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("state"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_jobs_row, state),
		.size = sizeof(((struct schema_memory_jobs_row *)0)->state),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
		.default_sql = NB_STR_LIT("'queued'"),
	},
	{
		.column = NB_STR_LIT("attempts"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_jobs_row, attempts),
		.size = sizeof(((struct schema_memory_jobs_row *)0)->attempts),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("worker_pid"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_jobs_row, worker_pid),
		.size = sizeof(((struct schema_memory_jobs_row *)0)->worker_pid),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("error_code"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_jobs_row, error_code),
		.size = sizeof(((struct schema_memory_jobs_row *)0)->error_code),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("updated_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_jobs_row, updated_at),
		.size = sizeof(((struct schema_memory_jobs_row *)0)->updated_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_kind = NORM_DEFAULT_CURRENT_EPOCH,
	},
	{
		.column = NB_STR_LIT("content_revision"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_jobs_row, content_revision),
		.size = sizeof(((struct schema_memory_jobs_row *)0)->content_revision),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("-1"),
	},
	{
		.column = NB_STR_LIT("stage"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_jobs_row, stage),
		.size = sizeof(((struct schema_memory_jobs_row *)0)->stage),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
		.default_sql = NB_STR_LIT("'extract'"),
	},
	{
		.column = NB_STR_LIT("legacy_error_code"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_jobs_row, legacy_error_code),
		.size = sizeof(((struct schema_memory_jobs_row *)0)->legacy_error_code),
		.null_offset = offsetof(struct schema_memory_jobs_row, legacy_error_code_missing),
		.flags = NORM_FIELD_NULLABLE,
	},
};

static const size_t memory_jobs_fk_0_fields[] = {1};
static const size_t memory_jobs_fk_0_references[] = {0};
static const struct norm_foreign_key memory_jobs_foreign_keys[] = {
	{.name = NB_STR_LIT("memory_jobs_fk_0"),
	 .fields = memory_jobs_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(memory_jobs_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = memory_jobs_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

struct schema_sub_agent_tasks_row {
	nb_str task_id;
	int64_t parent_session_id;
	bool parent_session_id_missing;
	int64_t child_session_id;
	bool child_session_id_missing;
	nb_str agent_name;
	nb_str description;
	nb_str mode;
	int64_t status;
	nb_str result;
	bool result_missing;
	int64_t error_code;
	int64_t iterations;
	int64_t started_at;
	int64_t ended_at;
	int64_t legacy_error_code;
	bool legacy_error_code_missing;
};

static const struct norm_field sub_agent_tasks_fields[] = {
	{
		.column = NB_STR_LIT("task_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_sub_agent_tasks_row, task_id),
		.size = sizeof(((struct schema_sub_agent_tasks_row *)0)->task_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("parent_session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_sub_agent_tasks_row, parent_session_id),
		.size = sizeof(((struct schema_sub_agent_tasks_row *)0)->parent_session_id),
		.null_offset =
			offsetof(struct schema_sub_agent_tasks_row, parent_session_id_missing),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("child_session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_sub_agent_tasks_row, child_session_id),
		.size = sizeof(((struct schema_sub_agent_tasks_row *)0)->child_session_id),
		.null_offset =
			offsetof(struct schema_sub_agent_tasks_row, child_session_id_missing),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("agent_name"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_sub_agent_tasks_row, agent_name),
		.size = sizeof(((struct schema_sub_agent_tasks_row *)0)->agent_name),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("description"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_sub_agent_tasks_row, description),
		.size = sizeof(((struct schema_sub_agent_tasks_row *)0)->description),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("mode"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_sub_agent_tasks_row, mode),
		.size = sizeof(((struct schema_sub_agent_tasks_row *)0)->mode),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("status"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_sub_agent_tasks_row, status),
		.size = sizeof(((struct schema_sub_agent_tasks_row *)0)->status),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("result"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_sub_agent_tasks_row, result),
		.size = sizeof(((struct schema_sub_agent_tasks_row *)0)->result),
		.null_offset = offsetof(struct schema_sub_agent_tasks_row, result_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("error_code"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_sub_agent_tasks_row, error_code),
		.size = sizeof(((struct schema_sub_agent_tasks_row *)0)->error_code),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("iterations"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_sub_agent_tasks_row, iterations),
		.size = sizeof(((struct schema_sub_agent_tasks_row *)0)->iterations),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("started_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_sub_agent_tasks_row, started_at),
		.size = sizeof(((struct schema_sub_agent_tasks_row *)0)->started_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("ended_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_sub_agent_tasks_row, ended_at),
		.size = sizeof(((struct schema_sub_agent_tasks_row *)0)->ended_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("legacy_error_code"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_sub_agent_tasks_row, legacy_error_code),
		.size = sizeof(((struct schema_sub_agent_tasks_row *)0)->legacy_error_code),
		.null_offset =
			offsetof(struct schema_sub_agent_tasks_row, legacy_error_code_missing),
		.flags = NORM_FIELD_NULLABLE,
	},
};

static const size_t sub_agent_tasks_fk_0_fields[] = {2};
static const size_t sub_agent_tasks_fk_0_references[] = {0};
static const size_t sub_agent_tasks_fk_1_fields[] = {1};
static const size_t sub_agent_tasks_fk_1_references[] = {0};
static const struct norm_foreign_key sub_agent_tasks_foreign_keys[] = {
	{.name = NB_STR_LIT("sub_agent_tasks_fk_0"),
	 .fields = sub_agent_tasks_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(sub_agent_tasks_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = sub_agent_tasks_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
	{.name = NB_STR_LIT("sub_agent_tasks_fk_1"),
	 .fields = sub_agent_tasks_fk_1_fields,
	 .field_count = NB_ARRAY_SIZE(sub_agent_tasks_fk_1_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = sub_agent_tasks_fk_1_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

static const size_t idx_sub_agent_tasks_parent_fields[] = {1, 10};
static const enum norm_sort_direction idx_sub_agent_tasks_parent_directions[] = {NORM_ASC,
										 NORM_ASC};
static const struct norm_index sub_agent_tasks_indexes[] = {
	{.name = NB_STR_LIT("idx_sub_agent_tasks_parent"),
	 .fields = idx_sub_agent_tasks_parent_fields,
	 .field_count = NB_ARRAY_SIZE(idx_sub_agent_tasks_parent_fields),
	 .unique = false,
	 .directions = idx_sub_agent_tasks_parent_directions},
};

struct schema_sub_agent_events_row {
	int64_t id;
	nb_str task_id;
	nb_str event_json;
	int64_t created_at;
};

static const struct norm_field sub_agent_events_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_sub_agent_events_row, id),
		.size = sizeof(((struct schema_sub_agent_events_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("task_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_sub_agent_events_row, task_id),
		.size = sizeof(((struct schema_sub_agent_events_row *)0)->task_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("event_json"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_sub_agent_events_row, event_json),
		.size = sizeof(((struct schema_sub_agent_events_row *)0)->event_json),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_sub_agent_events_row, created_at),
		.size = sizeof(((struct schema_sub_agent_events_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

static const size_t sub_agent_events_fk_0_fields[] = {1};
static const size_t sub_agent_events_fk_0_references[] = {0};
static const struct norm_foreign_key sub_agent_events_foreign_keys[] = {
	{.name = NB_STR_LIT("sub_agent_events_fk_0"),
	 .fields = sub_agent_events_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(sub_agent_events_fk_0_fields),
	 .references = &tables[SCHEMA_SUB_AGENT_TASKS],
	 .reference_fields = sub_agent_events_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

static const size_t idx_sub_agent_events_task_fields[] = {1, 0};
static const enum norm_sort_direction idx_sub_agent_events_task_directions[] = {NORM_ASC, NORM_ASC};
static const struct norm_index sub_agent_events_indexes[] = {
	{.name = NB_STR_LIT("idx_sub_agent_events_task"),
	 .fields = idx_sub_agent_events_task_fields,
	 .field_count = NB_ARRAY_SIZE(idx_sub_agent_events_task_fields),
	 .unique = false,
	 .directions = idx_sub_agent_events_task_directions},
};

struct schema_agent_threads_row {
	int64_t root_session_id;
	nb_str agent_id;
	nb_str parent_agent_id;
	int64_t session_id;
	nb_str model;
	nb_str effort;
	int64_t depth;
	nb_str description;
	nb_str assignment_json;
	bool assignment_json_missing;
	int64_t status;
	nb_str result;
	bool result_missing;
	int64_t error_code;
	int64_t iterations;
	int64_t started_at;
	int64_t ended_at;
	int64_t legacy_error_code;
	bool legacy_error_code_missing;
};

static const struct norm_field agent_threads_fields[] = {
	{
		.column = NB_STR_LIT("root_session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_threads_row, root_session_id),
		.size = sizeof(((struct schema_agent_threads_row *)0)->root_session_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("agent_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_agent_threads_row, agent_id),
		.size = sizeof(((struct schema_agent_threads_row *)0)->agent_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("parent_agent_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_agent_threads_row, parent_agent_id),
		.size = sizeof(((struct schema_agent_threads_row *)0)->parent_agent_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_threads_row, session_id),
		.size = sizeof(((struct schema_agent_threads_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("model"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_agent_threads_row, model),
		.size = sizeof(((struct schema_agent_threads_row *)0)->model),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("effort"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_agent_threads_row, effort),
		.size = sizeof(((struct schema_agent_threads_row *)0)->effort),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("depth"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_threads_row, depth),
		.size = sizeof(((struct schema_agent_threads_row *)0)->depth),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("description"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_agent_threads_row, description),
		.size = sizeof(((struct schema_agent_threads_row *)0)->description),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("assignment_json"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_agent_threads_row, assignment_json),
		.size = sizeof(((struct schema_agent_threads_row *)0)->assignment_json),
		.null_offset = offsetof(struct schema_agent_threads_row, assignment_json_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("status"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_threads_row, status),
		.size = sizeof(((struct schema_agent_threads_row *)0)->status),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("result"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_agent_threads_row, result),
		.size = sizeof(((struct schema_agent_threads_row *)0)->result),
		.null_offset = offsetof(struct schema_agent_threads_row, result_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("error_code"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_threads_row, error_code),
		.size = sizeof(((struct schema_agent_threads_row *)0)->error_code),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("iterations"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_threads_row, iterations),
		.size = sizeof(((struct schema_agent_threads_row *)0)->iterations),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("started_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_threads_row, started_at),
		.size = sizeof(((struct schema_agent_threads_row *)0)->started_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("ended_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_threads_row, ended_at),
		.size = sizeof(((struct schema_agent_threads_row *)0)->ended_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("legacy_error_code"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_threads_row, legacy_error_code),
		.size = sizeof(((struct schema_agent_threads_row *)0)->legacy_error_code),
		.null_offset = offsetof(struct schema_agent_threads_row, legacy_error_code_missing),
		.flags = NORM_FIELD_NULLABLE,
	},
};

static const size_t agent_threads_fk_0_fields[] = {3};
static const size_t agent_threads_fk_0_references[] = {0};
static const size_t agent_threads_fk_1_fields[] = {0};
static const size_t agent_threads_fk_1_references[] = {0};
static const struct norm_foreign_key agent_threads_foreign_keys[] = {
	{.name = NB_STR_LIT("agent_threads_fk_0"),
	 .fields = agent_threads_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(agent_threads_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = agent_threads_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
	{.name = NB_STR_LIT("agent_threads_fk_1"),
	 .fields = agent_threads_fk_1_fields,
	 .field_count = NB_ARRAY_SIZE(agent_threads_fk_1_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = agent_threads_fk_1_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

struct schema_agent_mailbox_row {
	int64_t id;
	int64_t root_session_id;
	nb_str sender;
	nb_str recipient;
	nb_str kind;
	nb_str body;
	int64_t consumed;
};

static const struct norm_field agent_mailbox_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_mailbox_row, id),
		.size = sizeof(((struct schema_agent_mailbox_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("root_session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_mailbox_row, root_session_id),
		.size = sizeof(((struct schema_agent_mailbox_row *)0)->root_session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("sender"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_agent_mailbox_row, sender),
		.size = sizeof(((struct schema_agent_mailbox_row *)0)->sender),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("recipient"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_agent_mailbox_row, recipient),
		.size = sizeof(((struct schema_agent_mailbox_row *)0)->recipient),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("kind"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_agent_mailbox_row, kind),
		.size = sizeof(((struct schema_agent_mailbox_row *)0)->kind),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("body"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_agent_mailbox_row, body),
		.size = sizeof(((struct schema_agent_mailbox_row *)0)->body),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("consumed"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_mailbox_row, consumed),
		.size = sizeof(((struct schema_agent_mailbox_row *)0)->consumed),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
};

static const size_t agent_mailbox_fk_0_fields[] = {1};
static const size_t agent_mailbox_fk_0_references[] = {0};
static const struct norm_foreign_key agent_mailbox_foreign_keys[] = {
	{.name = NB_STR_LIT("agent_mailbox_fk_0"),
	 .fields = agent_mailbox_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(agent_mailbox_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = agent_mailbox_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

static const size_t idx_agent_mailbox_recipient_fields[] = {1, 3, 6, 0};
static const enum norm_sort_direction idx_agent_mailbox_recipient_directions[] = {
	NORM_ASC, NORM_ASC, NORM_ASC, NORM_ASC};
static const struct norm_index agent_mailbox_indexes[] = {
	{.name = NB_STR_LIT("idx_agent_mailbox_recipient"),
	 .fields = idx_agent_mailbox_recipient_fields,
	 .field_count = NB_ARRAY_SIZE(idx_agent_mailbox_recipient_fields),
	 .unique = false,
	 .directions = idx_agent_mailbox_recipient_directions},
};

struct schema_agent_events_row {
	int64_t id;
	int64_t root_session_id;
	nb_str agent_id;
	nb_str event_json;
};

static const struct norm_field agent_events_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_events_row, id),
		.size = sizeof(((struct schema_agent_events_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("root_session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_events_row, root_session_id),
		.size = sizeof(((struct schema_agent_events_row *)0)->root_session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("agent_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_agent_events_row, agent_id),
		.size = sizeof(((struct schema_agent_events_row *)0)->agent_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("event_json"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_agent_events_row, event_json),
		.size = sizeof(((struct schema_agent_events_row *)0)->event_json),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
};

static const size_t agent_events_fk_0_fields[] = {1};
static const size_t agent_events_fk_0_references[] = {0};
static const struct norm_foreign_key agent_events_foreign_keys[] = {
	{.name = NB_STR_LIT("agent_events_fk_0"),
	 .fields = agent_events_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(agent_events_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = agent_events_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

struct schema_agent_budget_state_row {
	int64_t root_session_id;
	int64_t used;
	int64_t input_tokens;
	int64_t cached_tokens;
	int64_t output_tokens;
	int64_t reminder_level;
};

static const struct norm_field agent_budget_state_fields[] = {
	{
		.column = NB_STR_LIT("root_session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_budget_state_row, root_session_id),
		.size = sizeof(((struct schema_agent_budget_state_row *)0)->root_session_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("used"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_budget_state_row, used),
		.size = sizeof(((struct schema_agent_budget_state_row *)0)->used),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("input_tokens"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_budget_state_row, input_tokens),
		.size = sizeof(((struct schema_agent_budget_state_row *)0)->input_tokens),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("cached_tokens"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_budget_state_row, cached_tokens),
		.size = sizeof(((struct schema_agent_budget_state_row *)0)->cached_tokens),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("output_tokens"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_budget_state_row, output_tokens),
		.size = sizeof(((struct schema_agent_budget_state_row *)0)->output_tokens),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("reminder_level"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_agent_budget_state_row, reminder_level),
		.size = sizeof(((struct schema_agent_budget_state_row *)0)->reminder_level),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
};

static const size_t agent_budget_state_fk_0_fields[] = {0};
static const size_t agent_budget_state_fk_0_references[] = {0};
static const struct norm_foreign_key agent_budget_state_foreign_keys[] = {
	{.name = NB_STR_LIT("agent_budget_state_fk_0"),
	 .fields = agent_budget_state_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(agent_budget_state_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = agent_budget_state_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

struct schema_model_history_items_row {
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
	int64_t token_count;
	int64_t truncated;
	int64_t active;
	int64_t created_at;
};

static const struct norm_field model_history_items_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_model_history_items_row, id),
		.size = sizeof(((struct schema_model_history_items_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_model_history_items_row, session_id),
		.size = sizeof(((struct schema_model_history_items_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("sequence_no"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_model_history_items_row, sequence_no),
		.size = sizeof(((struct schema_model_history_items_row *)0)->sequence_no),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("turn_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_model_history_items_row, turn_id),
		.size = sizeof(((struct schema_model_history_items_row *)0)->turn_id),
		.null_offset = offsetof(struct schema_model_history_items_row, turn_id_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("kind"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_model_history_items_row, kind),
		.size = sizeof(((struct schema_model_history_items_row *)0)->kind),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("role"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_model_history_items_row, role),
		.size = sizeof(((struct schema_model_history_items_row *)0)->role),
		.null_offset = offsetof(struct schema_model_history_items_row, role_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("content"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_model_history_items_row, content),
		.size = sizeof(((struct schema_model_history_items_row *)0)->content),
		.null_offset = offsetof(struct schema_model_history_items_row, content_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("payload_json"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_model_history_items_row, payload_json),
		.size = sizeof(((struct schema_model_history_items_row *)0)->payload_json),
		.null_offset =
			offsetof(struct schema_model_history_items_row, payload_json_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("tool_call_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_model_history_items_row, tool_call_id),
		.size = sizeof(((struct schema_model_history_items_row *)0)->tool_call_id),
		.null_offset =
			offsetof(struct schema_model_history_items_row, tool_call_id_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("provider_call_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_model_history_items_row, provider_call_id),
		.size = sizeof(((struct schema_model_history_items_row *)0)->provider_call_id),
		.null_offset =
			offsetof(struct schema_model_history_items_row, provider_call_id_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("tool_name"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_model_history_items_row, tool_name),
		.size = sizeof(((struct schema_model_history_items_row *)0)->tool_name),
		.null_offset = offsetof(struct schema_model_history_items_row, tool_name_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("idempotency_key"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_model_history_items_row, idempotency_key),
		.size = sizeof(((struct schema_model_history_items_row *)0)->idempotency_key),
		.null_offset =
			offsetof(struct schema_model_history_items_row, idempotency_key_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("token_count"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_model_history_items_row, token_count),
		.size = sizeof(((struct schema_model_history_items_row *)0)->token_count),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("truncated"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_model_history_items_row, truncated),
		.size = sizeof(((struct schema_model_history_items_row *)0)->truncated),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("active"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_model_history_items_row, active),
		.size = sizeof(((struct schema_model_history_items_row *)0)->active),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("1"),
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_model_history_items_row, created_at),
		.size = sizeof(((struct schema_model_history_items_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

static const size_t model_history_items_fk_0_fields[] = {1};
static const size_t model_history_items_fk_0_references[] = {0};
static const struct norm_foreign_key model_history_items_foreign_keys[] = {
	{.name = NB_STR_LIT("model_history_items_fk_0"),
	 .fields = model_history_items_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(model_history_items_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = model_history_items_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

static const size_t idx_model_history_idempotency_fields[] = {1, 11};
static const enum norm_sort_direction idx_model_history_idempotency_directions[] = {NORM_ASC,
										    NORM_ASC};
static const struct norm_expr idx_model_history_idempotency_predicate = {
	.kind = NORM_EXPR_IS_NOT_NULL,
	.as.null_column = {.model = &tables[SCHEMA_MODEL_HISTORY_ITEMS], .index = 11}};
static const size_t idx_model_history_call_fields[] = {1, 8};
static const enum norm_sort_direction idx_model_history_call_directions[] = {NORM_ASC, NORM_ASC};
static const size_t idx_model_history_turn_fields[] = {1, 3, 2};
static const enum norm_sort_direction idx_model_history_turn_directions[] = {NORM_ASC, NORM_ASC,
									     NORM_ASC};
static const size_t idx_model_history_active_fields[] = {1, 14, 2};
static const enum norm_sort_direction idx_model_history_active_directions[] = {NORM_ASC, NORM_ASC,
									       NORM_ASC};
static const size_t sqlite_autoindex_model_history_items_1_fields[] = {1, 2};
static const enum norm_sort_direction sqlite_autoindex_model_history_items_1_directions[] = {
	NORM_ASC, NORM_ASC};
static const struct norm_index model_history_items_indexes[] = {
	{.name = NB_STR_LIT("idx_model_history_idempotency"),
	 .fields = idx_model_history_idempotency_fields,
	 .field_count = NB_ARRAY_SIZE(idx_model_history_idempotency_fields),
	 .unique = true,
	 .directions = idx_model_history_idempotency_directions,
	 .predicate = &idx_model_history_idempotency_predicate},
	{.name = NB_STR_LIT("idx_model_history_call"),
	 .fields = idx_model_history_call_fields,
	 .field_count = NB_ARRAY_SIZE(idx_model_history_call_fields),
	 .unique = false,
	 .directions = idx_model_history_call_directions},
	{.name = NB_STR_LIT("idx_model_history_turn"),
	 .fields = idx_model_history_turn_fields,
	 .field_count = NB_ARRAY_SIZE(idx_model_history_turn_fields),
	 .unique = false,
	 .directions = idx_model_history_turn_directions},
	{.name = NB_STR_LIT("idx_model_history_active"),
	 .fields = idx_model_history_active_fields,
	 .field_count = NB_ARRAY_SIZE(idx_model_history_active_fields),
	 .unique = false,
	 .directions = idx_model_history_active_directions},
	{.name = NB_STR_LIT("uq_model_history_items_session_id_sequence_no"),
	 .fields = sqlite_autoindex_model_history_items_1_fields,
	 .field_count = NB_ARRAY_SIZE(sqlite_autoindex_model_history_items_1_fields),
	 .unique = true,
	 .directions = sqlite_autoindex_model_history_items_1_directions},
};

struct schema_history_compactions_row {
	int64_t id;
	int64_t session_id;
	nb_str turn_id;
	bool turn_id_missing;
	int64_t cutoff_sequence_no;
	int64_t summary_item_id;
	int64_t input_tokens;
	int64_t output_tokens;
	nb_str trigger_kind;
	int64_t created_at;
};

static const struct norm_field history_compactions_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_history_compactions_row, id),
		.size = sizeof(((struct schema_history_compactions_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_history_compactions_row, session_id),
		.size = sizeof(((struct schema_history_compactions_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("turn_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_history_compactions_row, turn_id),
		.size = sizeof(((struct schema_history_compactions_row *)0)->turn_id),
		.null_offset = offsetof(struct schema_history_compactions_row, turn_id_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("cutoff_sequence_no"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_history_compactions_row, cutoff_sequence_no),
		.size = sizeof(((struct schema_history_compactions_row *)0)->cutoff_sequence_no),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("summary_item_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_history_compactions_row, summary_item_id),
		.size = sizeof(((struct schema_history_compactions_row *)0)->summary_item_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("input_tokens"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_history_compactions_row, input_tokens),
		.size = sizeof(((struct schema_history_compactions_row *)0)->input_tokens),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("output_tokens"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_history_compactions_row, output_tokens),
		.size = sizeof(((struct schema_history_compactions_row *)0)->output_tokens),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("trigger_kind"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_history_compactions_row, trigger_kind),
		.size = sizeof(((struct schema_history_compactions_row *)0)->trigger_kind),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_history_compactions_row, created_at),
		.size = sizeof(((struct schema_history_compactions_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

static const size_t history_compactions_fk_0_fields[] = {4};
static const size_t history_compactions_fk_0_references[] = {0};
static const size_t history_compactions_fk_1_fields[] = {1};
static const size_t history_compactions_fk_1_references[] = {0};
static const struct norm_foreign_key history_compactions_foreign_keys[] = {
	{.name = NB_STR_LIT("history_compactions_fk_0"),
	 .fields = history_compactions_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(history_compactions_fk_0_fields),
	 .references = &tables[SCHEMA_MODEL_HISTORY_ITEMS],
	 .reference_fields = history_compactions_fk_0_references,
	 .on_delete = NORM_FK_NO_ACTION,
	 .on_update = NORM_FK_NO_ACTION},
	{.name = NB_STR_LIT("history_compactions_fk_1"),
	 .fields = history_compactions_fk_1_fields,
	 .field_count = NB_ARRAY_SIZE(history_compactions_fk_1_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = history_compactions_fk_1_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

struct schema_history_summary_sources_row {
	int64_t summary_item_id;
	int64_t source_item_id;
};

static const struct norm_field history_summary_sources_fields[] = {
	{
		.column = NB_STR_LIT("summary_item_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_history_summary_sources_row, summary_item_id),
		.size = sizeof(((struct schema_history_summary_sources_row *)0)->summary_item_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("source_item_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_history_summary_sources_row, source_item_id),
		.size = sizeof(((struct schema_history_summary_sources_row *)0)->source_item_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
};

static const size_t history_summary_sources_fk_0_fields[] = {1};
static const size_t history_summary_sources_fk_0_references[] = {0};
static const size_t history_summary_sources_fk_1_fields[] = {0};
static const size_t history_summary_sources_fk_1_references[] = {0};
static const struct norm_foreign_key history_summary_sources_foreign_keys[] = {
	{.name = NB_STR_LIT("history_summary_sources_fk_0"),
	 .fields = history_summary_sources_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(history_summary_sources_fk_0_fields),
	 .references = &tables[SCHEMA_MODEL_HISTORY_ITEMS],
	 .reference_fields = history_summary_sources_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
	{.name = NB_STR_LIT("history_summary_sources_fk_1"),
	 .fields = history_summary_sources_fk_1_fields,
	 .field_count = NB_ARRAY_SIZE(history_summary_sources_fk_1_fields),
	 .references = &tables[SCHEMA_MODEL_HISTORY_ITEMS],
	 .reference_fields = history_summary_sources_fk_1_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

struct schema_model_request_snapshots_row {
	int64_t session_id;
	nb_str turn_id;
	bool turn_id_missing;
	nb_str provider;
	nb_str model;
	nb_str input_json;
	int64_t created_at;
};

static const struct norm_field model_request_snapshots_fields[] = {
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_model_request_snapshots_row, session_id),
		.size = sizeof(((struct schema_model_request_snapshots_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("turn_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_model_request_snapshots_row, turn_id),
		.size = sizeof(((struct schema_model_request_snapshots_row *)0)->turn_id),
		.null_offset = offsetof(struct schema_model_request_snapshots_row, turn_id_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("provider"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_model_request_snapshots_row, provider),
		.size = sizeof(((struct schema_model_request_snapshots_row *)0)->provider),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("model"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_model_request_snapshots_row, model),
		.size = sizeof(((struct schema_model_request_snapshots_row *)0)->model),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("input_json"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_model_request_snapshots_row, input_json),
		.size = sizeof(((struct schema_model_request_snapshots_row *)0)->input_json),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_model_request_snapshots_row, created_at),
		.size = sizeof(((struct schema_model_request_snapshots_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

static const size_t model_request_snapshots_fk_0_fields[] = {0};
static const size_t model_request_snapshots_fk_0_references[] = {0};
static const struct norm_foreign_key model_request_snapshots_foreign_keys[] = {
	{.name = NB_STR_LIT("model_request_snapshots_fk_0"),
	 .fields = model_request_snapshots_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(model_request_snapshots_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = model_request_snapshots_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

struct schema_history_compaction_attempts_row {
	int64_t id;
	int64_t session_id;
	nb_str turn_id;
	bool turn_id_missing;
	nb_str trigger_kind;
	nb_str status;
	int64_t input_tokens;
	int64_t output_tokens;
	int64_t error_code;
	nb_str error_text;
	bool error_text_missing;
	int64_t created_at;
	int64_t legacy_error_code;
	bool legacy_error_code_missing;
};

static const struct norm_field history_compaction_attempts_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_history_compaction_attempts_row, id),
		.size = sizeof(((struct schema_history_compaction_attempts_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_history_compaction_attempts_row, session_id),
		.size = sizeof(((struct schema_history_compaction_attempts_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("turn_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_history_compaction_attempts_row, turn_id),
		.size = sizeof(((struct schema_history_compaction_attempts_row *)0)->turn_id),
		.null_offset =
			offsetof(struct schema_history_compaction_attempts_row, turn_id_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("trigger_kind"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_history_compaction_attempts_row, trigger_kind),
		.size = sizeof(((struct schema_history_compaction_attempts_row *)0)->trigger_kind),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("status"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_history_compaction_attempts_row, status),
		.size = sizeof(((struct schema_history_compaction_attempts_row *)0)->status),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("input_tokens"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_history_compaction_attempts_row, input_tokens),
		.size = sizeof(((struct schema_history_compaction_attempts_row *)0)->input_tokens),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("output_tokens"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_history_compaction_attempts_row, output_tokens),
		.size = sizeof(((struct schema_history_compaction_attempts_row *)0)->output_tokens),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("error_code"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_history_compaction_attempts_row, error_code),
		.size = sizeof(((struct schema_history_compaction_attempts_row *)0)->error_code),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("error_text"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_history_compaction_attempts_row, error_text),
		.size = sizeof(((struct schema_history_compaction_attempts_row *)0)->error_text),
		.null_offset =
			offsetof(struct schema_history_compaction_attempts_row, error_text_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_history_compaction_attempts_row, created_at),
		.size = sizeof(((struct schema_history_compaction_attempts_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("legacy_error_code"),
		.type = NORM_TYPE_I64,
		.offset =
			offsetof(struct schema_history_compaction_attempts_row, legacy_error_code),
		.size = sizeof(
			((struct schema_history_compaction_attempts_row *)0)->legacy_error_code),
		.null_offset = offsetof(struct schema_history_compaction_attempts_row,
					legacy_error_code_missing),
		.flags = NORM_FIELD_NULLABLE,
	},
};

static const size_t history_compaction_attempts_fk_0_fields[] = {1};
static const size_t history_compaction_attempts_fk_0_references[] = {0};
static const struct norm_foreign_key history_compaction_attempts_foreign_keys[] = {
	{.name = NB_STR_LIT("history_compaction_attempts_fk_0"),
	 .fields = history_compaction_attempts_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(history_compaction_attempts_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = history_compaction_attempts_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

static const size_t idx_history_compaction_attempts_session_fields[] = {1, 9, 0};
static const enum norm_sort_direction idx_history_compaction_attempts_session_directions[] = {
	NORM_ASC, NORM_ASC, NORM_ASC};
static const struct norm_index history_compaction_attempts_indexes[] = {
	{.name = NB_STR_LIT("idx_history_compaction_attempts_session"),
	 .fields = idx_history_compaction_attempts_session_fields,
	 .field_count = NB_ARRAY_SIZE(idx_history_compaction_attempts_session_fields),
	 .unique = false,
	 .directions = idx_history_compaction_attempts_session_directions},
};

struct schema_session_plans_row {
	int64_t session_id;
	nb_str state_json;
};

static const struct norm_field session_plans_fields[] = {
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_session_plans_row, session_id),
		.size = sizeof(((struct schema_session_plans_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("state_json"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_session_plans_row, state_json),
		.size = sizeof(((struct schema_session_plans_row *)0)->state_json),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
};

static const size_t session_plans_fk_0_fields[] = {0};
static const size_t session_plans_fk_0_references[] = {0};
static const struct norm_foreign_key session_plans_foreign_keys[] = {
	{.name = NB_STR_LIT("session_plans_fk_0"),
	 .fields = session_plans_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(session_plans_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = session_plans_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

struct schema_permission_grants_row {
	int64_t id;
	nb_str subject;
	nb_str resource_kind;
	nb_str resource;
	nb_str project_root;
	int64_t created_at;
};

static const struct norm_field permission_grants_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_permission_grants_row, id),
		.size = sizeof(((struct schema_permission_grants_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("subject"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_permission_grants_row, subject),
		.size = sizeof(((struct schema_permission_grants_row *)0)->subject),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("resource_kind"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_permission_grants_row, resource_kind),
		.size = sizeof(((struct schema_permission_grants_row *)0)->resource_kind),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("resource"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_permission_grants_row, resource),
		.size = sizeof(((struct schema_permission_grants_row *)0)->resource),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("project_root"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_permission_grants_row, project_root),
		.size = sizeof(((struct schema_permission_grants_row *)0)->project_root),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_permission_grants_row, created_at),
		.size = sizeof(((struct schema_permission_grants_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

static const size_t idx_permission_grants_project_fields[] = {4, 1, 2};
static const enum norm_sort_direction idx_permission_grants_project_directions[] = {
	NORM_ASC, NORM_ASC, NORM_ASC};
static const size_t sqlite_autoindex_permission_grants_1_fields[] = {1, 2, 3, 4};
static const enum norm_sort_direction sqlite_autoindex_permission_grants_1_directions[] = {
	NORM_ASC, NORM_ASC, NORM_ASC, NORM_ASC};
static const struct norm_index permission_grants_indexes[] = {
	{.name = NB_STR_LIT("idx_permission_grants_project"),
	 .fields = idx_permission_grants_project_fields,
	 .field_count = NB_ARRAY_SIZE(idx_permission_grants_project_fields),
	 .unique = false,
	 .directions = idx_permission_grants_project_directions},
	{.name = NB_STR_LIT("uq_permission_grants_identity"),
	 .fields = sqlite_autoindex_permission_grants_1_fields,
	 .field_count = NB_ARRAY_SIZE(sqlite_autoindex_permission_grants_1_fields),
	 .unique = true,
	 .directions = sqlite_autoindex_permission_grants_1_directions},
};

struct schema_scheduled_tasks_row {
	int64_t id;
	int64_t source_session_id;
	bool source_session_id_missing;
	int64_t latest_session_id;
	bool latest_session_id_missing;
	nb_str title;
	nb_str kind;
	nb_str status;
	nb_str trigger_type;
	int64_t next_run_at;
	int64_t interval_seconds;
	int64_t timeout_at;
	int64_t attempts;
	int64_t max_attempts;
	nb_str action_type;
	nb_str payload_json;
	bool payload_json_missing;
	nb_str policy_json;
	bool policy_json_missing;
	nb_str notify_json;
	bool notify_json_missing;
	nb_str last_error;
	bool last_error_missing;
	int64_t created_at;
	int64_t updated_at;
	nb_str run_token;
};

static const struct norm_field scheduled_tasks_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_scheduled_tasks_row, id),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("source_session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_scheduled_tasks_row, source_session_id),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->source_session_id),
		.null_offset =
			offsetof(struct schema_scheduled_tasks_row, source_session_id_missing),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("latest_session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_scheduled_tasks_row, latest_session_id),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->latest_session_id),
		.null_offset =
			offsetof(struct schema_scheduled_tasks_row, latest_session_id_missing),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("title"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_scheduled_tasks_row, title),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->title),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("kind"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_scheduled_tasks_row, kind),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->kind),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("status"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_scheduled_tasks_row, status),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->status),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("trigger_type"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_scheduled_tasks_row, trigger_type),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->trigger_type),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("next_run_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_scheduled_tasks_row, next_run_at),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->next_run_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("interval_seconds"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_scheduled_tasks_row, interval_seconds),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->interval_seconds),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("timeout_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_scheduled_tasks_row, timeout_at),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->timeout_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("attempts"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_scheduled_tasks_row, attempts),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->attempts),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("max_attempts"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_scheduled_tasks_row, max_attempts),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->max_attempts),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("action_type"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_scheduled_tasks_row, action_type),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->action_type),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("payload_json"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_scheduled_tasks_row, payload_json),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->payload_json),
		.null_offset = offsetof(struct schema_scheduled_tasks_row, payload_json_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("policy_json"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_scheduled_tasks_row, policy_json),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->policy_json),
		.null_offset = offsetof(struct schema_scheduled_tasks_row, policy_json_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("notify_json"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_scheduled_tasks_row, notify_json),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->notify_json),
		.null_offset = offsetof(struct schema_scheduled_tasks_row, notify_json_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("last_error"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_scheduled_tasks_row, last_error),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->last_error),
		.null_offset = offsetof(struct schema_scheduled_tasks_row, last_error_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_scheduled_tasks_row, created_at),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("updated_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_scheduled_tasks_row, updated_at),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->updated_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("run_token"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_scheduled_tasks_row, run_token),
		.size = sizeof(((struct schema_scheduled_tasks_row *)0)->run_token),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
		.default_sql = NB_STR_LIT("''"),
	},
};

static const size_t scheduled_tasks_fk_0_fields[] = {2};
static const size_t scheduled_tasks_fk_0_references[] = {0};
static const size_t scheduled_tasks_fk_1_fields[] = {1};
static const size_t scheduled_tasks_fk_1_references[] = {0};
static const struct norm_foreign_key scheduled_tasks_foreign_keys[] = {
	{.name = NB_STR_LIT("scheduled_tasks_fk_0"),
	 .fields = scheduled_tasks_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(scheduled_tasks_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = scheduled_tasks_fk_0_references,
	 .on_delete = NORM_FK_SET_NULL,
	 .on_update = NORM_FK_NO_ACTION},
	{.name = NB_STR_LIT("scheduled_tasks_fk_1"),
	 .fields = scheduled_tasks_fk_1_fields,
	 .field_count = NB_ARRAY_SIZE(scheduled_tasks_fk_1_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = scheduled_tasks_fk_1_references,
	 .on_delete = NORM_FK_SET_NULL,
	 .on_update = NORM_FK_NO_ACTION},
};

static const size_t idx_scheduled_tasks_due_fields[] = {5, 7};
static const enum norm_sort_direction idx_scheduled_tasks_due_directions[] = {NORM_ASC, NORM_ASC};
static const struct norm_index scheduled_tasks_indexes[] = {
	{.name = NB_STR_LIT("idx_scheduled_tasks_due"),
	 .fields = idx_scheduled_tasks_due_fields,
	 .field_count = NB_ARRAY_SIZE(idx_scheduled_tasks_due_fields),
	 .unique = false,
	 .directions = idx_scheduled_tasks_due_directions},
};

struct schema_notifications_row {
	int64_t id;
	int64_t task_id;
	bool task_id_missing;
	int64_t session_id;
	bool session_id_missing;
	nb_str level;
	nb_str title;
	nb_str body;
	int64_t created_at;
	int64_t read_at;
	nb_str delivery_status;
};

static const struct norm_field notifications_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_notifications_row, id),
		.size = sizeof(((struct schema_notifications_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("task_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_notifications_row, task_id),
		.size = sizeof(((struct schema_notifications_row *)0)->task_id),
		.null_offset = offsetof(struct schema_notifications_row, task_id_missing),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_notifications_row, session_id),
		.size = sizeof(((struct schema_notifications_row *)0)->session_id),
		.null_offset = offsetof(struct schema_notifications_row, session_id_missing),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("level"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_notifications_row, level),
		.size = sizeof(((struct schema_notifications_row *)0)->level),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("title"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_notifications_row, title),
		.size = sizeof(((struct schema_notifications_row *)0)->title),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("body"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_notifications_row, body),
		.size = sizeof(((struct schema_notifications_row *)0)->body),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_notifications_row, created_at),
		.size = sizeof(((struct schema_notifications_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("read_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_notifications_row, read_at),
		.size = sizeof(((struct schema_notifications_row *)0)->read_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("delivery_status"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_notifications_row, delivery_status),
		.size = sizeof(((struct schema_notifications_row *)0)->delivery_status),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
};

static const size_t notifications_fk_0_fields[] = {2};
static const size_t notifications_fk_0_references[] = {0};
static const size_t notifications_fk_1_fields[] = {1};
static const size_t notifications_fk_1_references[] = {0};
static const struct norm_foreign_key notifications_foreign_keys[] = {
	{.name = NB_STR_LIT("notifications_fk_0"),
	 .fields = notifications_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(notifications_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = notifications_fk_0_references,
	 .on_delete = NORM_FK_SET_NULL,
	 .on_update = NORM_FK_NO_ACTION},
	{.name = NB_STR_LIT("notifications_fk_1"),
	 .fields = notifications_fk_1_fields,
	 .field_count = NB_ARRAY_SIZE(notifications_fk_1_fields),
	 .references = &tables[SCHEMA_SCHEDULED_TASKS],
	 .reference_fields = notifications_fk_1_references,
	 .on_delete = NORM_FK_SET_NULL,
	 .on_update = NORM_FK_NO_ACTION},
};

static const size_t idx_notifications_unread_fields[] = {7, 6};
static const enum norm_sort_direction idx_notifications_unread_directions[] = {NORM_ASC, NORM_ASC};
static const struct norm_index notifications_indexes[] = {
	{.name = NB_STR_LIT("idx_notifications_unread"),
	 .fields = idx_notifications_unread_fields,
	 .field_count = NB_ARRAY_SIZE(idx_notifications_unread_fields),
	 .unique = false,
	 .directions = idx_notifications_unread_directions},
};

struct schema_credit_events_row {
	int64_t id;
	nb_str user_id;
	bool user_id_missing;
	nb_str session_id;
	bool session_id_missing;
	nb_str kind;
	nb_str provider;
	bool provider_missing;
	nb_str model;
	bool model_missing;
	int64_t input_tokens;
	int64_t output_tokens;
	int64_t image_units;
	int64_t video_seconds;
	double estimated_cost;
	nb_str currency;
	int64_t credits;
	nb_str metadata_json;
	bool metadata_json_missing;
	int64_t created_at;
};

static const struct norm_field credit_events_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_credit_events_row, id),
		.size = sizeof(((struct schema_credit_events_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("user_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_credit_events_row, user_id),
		.size = sizeof(((struct schema_credit_events_row *)0)->user_id),
		.null_offset = offsetof(struct schema_credit_events_row, user_id_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_credit_events_row, session_id),
		.size = sizeof(((struct schema_credit_events_row *)0)->session_id),
		.null_offset = offsetof(struct schema_credit_events_row, session_id_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("kind"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_credit_events_row, kind),
		.size = sizeof(((struct schema_credit_events_row *)0)->kind),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("provider"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_credit_events_row, provider),
		.size = sizeof(((struct schema_credit_events_row *)0)->provider),
		.null_offset = offsetof(struct schema_credit_events_row, provider_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("model"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_credit_events_row, model),
		.size = sizeof(((struct schema_credit_events_row *)0)->model),
		.null_offset = offsetof(struct schema_credit_events_row, model_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("input_tokens"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_credit_events_row, input_tokens),
		.size = sizeof(((struct schema_credit_events_row *)0)->input_tokens),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("output_tokens"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_credit_events_row, output_tokens),
		.size = sizeof(((struct schema_credit_events_row *)0)->output_tokens),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("image_units"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_credit_events_row, image_units),
		.size = sizeof(((struct schema_credit_events_row *)0)->image_units),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("video_seconds"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_credit_events_row, video_seconds),
		.size = sizeof(((struct schema_credit_events_row *)0)->video_seconds),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("estimated_cost"),
		.type = NORM_TYPE_F64,
		.offset = offsetof(struct schema_credit_events_row, estimated_cost),
		.size = sizeof(((struct schema_credit_events_row *)0)->estimated_cost),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("currency"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_credit_events_row, currency),
		.size = sizeof(((struct schema_credit_events_row *)0)->currency),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
		.default_sql = NB_STR_LIT("'USD'"),
	},
	{
		.column = NB_STR_LIT("credits"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_credit_events_row, credits),
		.size = sizeof(((struct schema_credit_events_row *)0)->credits),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("metadata_json"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_credit_events_row, metadata_json),
		.size = sizeof(((struct schema_credit_events_row *)0)->metadata_json),
		.null_offset = offsetof(struct schema_credit_events_row, metadata_json_missing),
		.flags = NORM_FIELD_NULLABLE,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_credit_events_row, created_at),
		.size = sizeof(((struct schema_credit_events_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

static const size_t idx_credit_events_session_fields[] = {2, 14};
static const enum norm_sort_direction idx_credit_events_session_directions[] = {NORM_ASC, NORM_ASC};
static const size_t idx_credit_events_user_day_fields[] = {1, 14};
static const enum norm_sort_direction idx_credit_events_user_day_directions[] = {NORM_ASC,
										 NORM_ASC};
static const struct norm_index credit_events_indexes[] = {
	{.name = NB_STR_LIT("idx_credit_events_session"),
	 .fields = idx_credit_events_session_fields,
	 .field_count = NB_ARRAY_SIZE(idx_credit_events_session_fields),
	 .unique = false,
	 .directions = idx_credit_events_session_directions},
	{.name = NB_STR_LIT("idx_credit_events_user_day"),
	 .fields = idx_credit_events_user_day_fields,
	 .field_count = NB_ARRAY_SIZE(idx_credit_events_user_day_fields),
	 .unique = false,
	 .directions = idx_credit_events_user_day_directions},
};

struct schema_memory_notes_row {
	int64_t id;
	int64_t session_id;
	nb_str title;
	nb_str text;
	nb_str source;
	nb_str event_token;
	nb_str scope;
	int64_t created_at;
	int64_t updated_at;
};

static const struct norm_field memory_notes_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_notes_row, id),
		.size = sizeof(((struct schema_memory_notes_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_notes_row, session_id),
		.size = sizeof(((struct schema_memory_notes_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("title"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_notes_row, title),
		.size = sizeof(((struct schema_memory_notes_row *)0)->title),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("text"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_notes_row, text),
		.size = sizeof(((struct schema_memory_notes_row *)0)->text),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("source"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_notes_row, source),
		.size = sizeof(((struct schema_memory_notes_row *)0)->source),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("event_token"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_notes_row, event_token),
		.size = sizeof(((struct schema_memory_notes_row *)0)->event_token),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("scope"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_notes_row, scope),
		.size = sizeof(((struct schema_memory_notes_row *)0)->scope),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
		.default_sql = NB_STR_LIT("'project'"),
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_notes_row, created_at),
		.size = sizeof(((struct schema_memory_notes_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_kind = NORM_DEFAULT_CURRENT_EPOCH,
	},
	{
		.column = NB_STR_LIT("updated_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_notes_row, updated_at),
		.size = sizeof(((struct schema_memory_notes_row *)0)->updated_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_kind = NORM_DEFAULT_CURRENT_EPOCH,
	},
};

static const size_t memory_notes_fk_0_fields[] = {1};
static const size_t memory_notes_fk_0_references[] = {0};
static const struct norm_foreign_key memory_notes_foreign_keys[] = {
	{.name = NB_STR_LIT("memory_notes_fk_0"),
	 .fields = memory_notes_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(memory_notes_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = memory_notes_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

static const size_t idx_memory_notes_session_fields[] = {1, 0};
static const enum norm_sort_direction idx_memory_notes_session_directions[] = {NORM_ASC, NORM_ASC};
static const size_t sqlite_autoindex_memory_notes_1_fields[] = {1, 5, 2};
static const enum norm_sort_direction sqlite_autoindex_memory_notes_1_directions[] = {
	NORM_ASC, NORM_ASC, NORM_ASC};
static const struct norm_index memory_notes_indexes[] = {
	{.name = NB_STR_LIT("idx_memory_notes_session"),
	 .fields = idx_memory_notes_session_fields,
	 .field_count = NB_ARRAY_SIZE(idx_memory_notes_session_fields),
	 .unique = false,
	 .directions = idx_memory_notes_session_directions},
	{.name = NB_STR_LIT("uq_memory_notes_session_id_event_token_title"),
	 .fields = sqlite_autoindex_memory_notes_1_fields,
	 .field_count = NB_ARRAY_SIZE(sqlite_autoindex_memory_notes_1_fields),
	 .unique = true,
	 .directions = sqlite_autoindex_memory_notes_1_directions},
};

static const struct norm_value memory_notes_check_0_values[] = {
	{.type = NORM_TYPE_TEXT, .as.text = NB_STR_LIT("project")},
	{.type = NORM_TYPE_TEXT, .as.text = NB_STR_LIT("session")}};
static const struct norm_expr memory_notes_check_0_predicate = {
	.kind = NORM_EXPR_IN,
	.as.in = {.column = {.model = &tables[SCHEMA_MEMORY_NOTES], .index = 6},
		  .values = memory_notes_check_0_values,
		  .count = NB_ARRAY_SIZE(memory_notes_check_0_values)}};
static const struct norm_check memory_notes_checks[] = {
	{.name = NB_STR_LIT("memory_notes_check_0"), .predicate = &memory_notes_check_0_predicate},
};

struct schema_memory_summaries_row {
	int64_t session_id;
	int64_t memory_version;
	int64_t content_revision;
	nb_str title;
	nb_str text;
	nb_str sources;
	int64_t updated_at;
};

static const struct norm_field memory_summaries_fields[] = {
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_summaries_row, session_id),
		.size = sizeof(((struct schema_memory_summaries_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("memory_version"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_summaries_row, memory_version),
		.size = sizeof(((struct schema_memory_summaries_row *)0)->memory_version),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("content_revision"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_summaries_row, content_revision),
		.size = sizeof(((struct schema_memory_summaries_row *)0)->content_revision),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("title"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_summaries_row, title),
		.size = sizeof(((struct schema_memory_summaries_row *)0)->title),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("text"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_summaries_row, text),
		.size = sizeof(((struct schema_memory_summaries_row *)0)->text),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("sources"),
		.type = NORM_TYPE_TEXT,
		.offset = offsetof(struct schema_memory_summaries_row, sources),
		.size = sizeof(((struct schema_memory_summaries_row *)0)->sources),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.max_length = SIZE_MAX,
	},
	{
		.column = NB_STR_LIT("updated_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct schema_memory_summaries_row, updated_at),
		.size = sizeof(((struct schema_memory_summaries_row *)0)->updated_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_kind = NORM_DEFAULT_CURRENT_EPOCH,
	},
};

static const size_t memory_summaries_fk_0_fields[] = {0};
static const size_t memory_summaries_fk_0_references[] = {0};
static const struct norm_foreign_key memory_summaries_foreign_keys[] = {
	{.name = NB_STR_LIT("memory_summaries_fk_0"),
	 .fields = memory_summaries_fk_0_fields,
	 .field_count = NB_ARRAY_SIZE(memory_summaries_fk_0_fields),
	 .references = &tables[SCHEMA_SESSIONS],
	 .reference_fields = memory_summaries_fk_0_references,
	 .on_delete = NORM_FK_CASCADE,
	 .on_update = NORM_FK_NO_ACTION},
};

static const struct norm_model tables[SCHEMA_TABLE_COUNT] = {
	[SCHEMA_SCHEMA_MIGRATIONS] = {NORM_MODEL_METADATA(struct schema_schema_migrations_row,
							  "schema_migrations",
							  schema_migrations_fields)},
	[SCHEMA_SESSIONS] = {NORM_MODEL_METADATA(struct schema_sessions_row, "sessions",
						 sessions_fields)},
	[SCHEMA_SESSION_SKILL_ACTIVATIONS] =
		{NORM_MODEL_METADATA(struct schema_session_skill_activations_row,
				     "session_skill_activations", session_skill_activations_fields),
		 .foreign_keys = session_skill_activations_foreign_keys,
		 .foreign_key_count = NB_ARRAY_SIZE(session_skill_activations_foreign_keys)},
	[SCHEMA_MESSAGES] = {NORM_MODEL_METADATA(struct schema_messages_row, "messages",
						 messages_fields),
			     .foreign_keys = messages_foreign_keys,
			     .foreign_key_count = NB_ARRAY_SIZE(messages_foreign_keys)},
	[SCHEMA_MESSAGE_ATTACHMENTS] = {NORM_MODEL_METADATA(struct schema_message_attachments_row,
							    "message_attachments",
							    message_attachments_fields),
					.foreign_keys = message_attachments_foreign_keys,
					.foreign_key_count =
						NB_ARRAY_SIZE(message_attachments_foreign_keys)},
	[SCHEMA_REACT_TRACES] = {NORM_MODEL_METADATA(struct schema_react_traces_row, "react_traces",
						     react_traces_fields),
				 .foreign_keys = react_traces_foreign_keys,
				 .foreign_key_count = NB_ARRAY_SIZE(react_traces_foreign_keys)},
	[SCHEMA_EXTS] = {NORM_MODEL_METADATA(struct schema_exts_row, "exts", exts_fields)},
	[SCHEMA_OUTPUTS] = {NORM_MODEL_METADATA(struct schema_outputs_row, "outputs",
						outputs_fields),
			    .foreign_keys = outputs_foreign_keys,
			    .foreign_key_count = NB_ARRAY_SIZE(outputs_foreign_keys)},
	[SCHEMA_MEMORY_PROFILES] = {NORM_MODEL_METADATA(struct schema_memory_profiles_row,
							"memory_profiles", memory_profiles_fields),
				    .foreign_keys = memory_profiles_foreign_keys,
				    .foreign_key_count =
					    NB_ARRAY_SIZE(memory_profiles_foreign_keys)},
	[SCHEMA_MEMORY_FACTS] = {NORM_MODEL_METADATA(struct schema_memory_facts_row, "memory_facts",
						     memory_facts_fields),
				 .foreign_keys = memory_facts_foreign_keys,
				 .foreign_key_count = NB_ARRAY_SIZE(memory_facts_foreign_keys)},
	[SCHEMA_MEMORY_EPISODES] = {NORM_MODEL_METADATA(struct schema_memory_episodes_row,
							"memory_episodes", memory_episodes_fields),
				    .foreign_keys = memory_episodes_foreign_keys,
				    .foreign_key_count =
					    NB_ARRAY_SIZE(memory_episodes_foreign_keys)},
	[SCHEMA_MEMORY_PROCEDURES] = {NORM_MODEL_METADATA(struct schema_memory_procedures_row,
							  "memory_procedures",
							  memory_procedures_fields),
				      .foreign_keys = memory_procedures_foreign_keys,
				      .foreign_key_count =
					      NB_ARRAY_SIZE(memory_procedures_foreign_keys)},
	[SCHEMA_MEMORY_SCOPES] = {NORM_MODEL_METADATA(struct schema_memory_scopes_row,
						      "memory_scopes", memory_scopes_fields),
				  .foreign_keys = memory_scopes_foreign_keys,
				  .foreign_key_count = NB_ARRAY_SIZE(memory_scopes_foreign_keys)},
	[SCHEMA_MEMORY_PREFERENCES] = {NORM_MODEL_METADATA(struct schema_memory_preferences_row,
							   "memory_preferences",
							   memory_preferences_fields),
				       NORM_CHECKS(memory_preferences_checks)},
	[SCHEMA_MEMORY_CANDIDATES] = {NORM_MODEL_METADATA(struct schema_memory_candidates_row,
							  "memory_candidates",
							  memory_candidates_fields),
				      .foreign_keys = memory_candidates_foreign_keys,
				      .foreign_key_count =
					      NB_ARRAY_SIZE(memory_candidates_foreign_keys)},
	[SCHEMA_MEMORY_JOBS] = {NORM_MODEL_METADATA(struct schema_memory_jobs_row, "memory_jobs",
						    memory_jobs_fields),
				.foreign_keys = memory_jobs_foreign_keys,
				.foreign_key_count = NB_ARRAY_SIZE(memory_jobs_foreign_keys)},
	[SCHEMA_SUB_AGENT_TASKS] = {NORM_MODEL_METADATA(struct schema_sub_agent_tasks_row,
							"sub_agent_tasks", sub_agent_tasks_fields),
				    .foreign_keys = sub_agent_tasks_foreign_keys,
				    .foreign_key_count =
					    NB_ARRAY_SIZE(sub_agent_tasks_foreign_keys)},
	[SCHEMA_SUB_AGENT_EVENTS] = {NORM_MODEL_METADATA(struct schema_sub_agent_events_row,
							 "sub_agent_events",
							 sub_agent_events_fields),
				     .foreign_keys = sub_agent_events_foreign_keys,
				     .foreign_key_count =
					     NB_ARRAY_SIZE(sub_agent_events_foreign_keys)},
	[SCHEMA_AGENT_THREADS] = {NORM_MODEL_METADATA(struct schema_agent_threads_row,
						      "agent_threads", agent_threads_fields),
				  .foreign_keys = agent_threads_foreign_keys,
				  .foreign_key_count = NB_ARRAY_SIZE(agent_threads_foreign_keys)},
	[SCHEMA_AGENT_MAILBOX] = {NORM_MODEL_METADATA(struct schema_agent_mailbox_row,
						      "agent_mailbox", agent_mailbox_fields),
				  .foreign_keys = agent_mailbox_foreign_keys,
				  .foreign_key_count = NB_ARRAY_SIZE(agent_mailbox_foreign_keys)},
	[SCHEMA_AGENT_EVENTS] = {NORM_MODEL_METADATA(struct schema_agent_events_row, "agent_events",
						     agent_events_fields),
				 .foreign_keys = agent_events_foreign_keys,
				 .foreign_key_count = NB_ARRAY_SIZE(agent_events_foreign_keys)},
	[SCHEMA_AGENT_BUDGET_STATE] = {NORM_MODEL_METADATA(struct schema_agent_budget_state_row,
							   "agent_budget_state",
							   agent_budget_state_fields),
				       .foreign_keys = agent_budget_state_foreign_keys,
				       .foreign_key_count =
					       NB_ARRAY_SIZE(agent_budget_state_foreign_keys)},
	[SCHEMA_MODEL_HISTORY_ITEMS] = {NORM_MODEL_METADATA(struct schema_model_history_items_row,
							    "model_history_items",
							    model_history_items_fields),
					.foreign_keys = model_history_items_foreign_keys,
					.foreign_key_count =
						NB_ARRAY_SIZE(model_history_items_foreign_keys)},
	[SCHEMA_HISTORY_COMPACTIONS] = {NORM_MODEL_METADATA(struct schema_history_compactions_row,
							    "history_compactions",
							    history_compactions_fields),
					.foreign_keys = history_compactions_foreign_keys,
					.foreign_key_count =
						NB_ARRAY_SIZE(history_compactions_foreign_keys)},
	[SCHEMA_HISTORY_SUMMARY_SOURCES] =
		{NORM_MODEL_METADATA(struct schema_history_summary_sources_row,
				     "history_summary_sources", history_summary_sources_fields),
		 .foreign_keys = history_summary_sources_foreign_keys,
		 .foreign_key_count = NB_ARRAY_SIZE(history_summary_sources_foreign_keys)},
	[SCHEMA_MODEL_REQUEST_SNAPSHOTS] =
		{NORM_MODEL_METADATA(struct schema_model_request_snapshots_row,
				     "model_request_snapshots", model_request_snapshots_fields),
		 .foreign_keys = model_request_snapshots_foreign_keys,
		 .foreign_key_count = NB_ARRAY_SIZE(model_request_snapshots_foreign_keys)},
	[SCHEMA_HISTORY_COMPACTION_ATTEMPTS] =
		{NORM_MODEL_METADATA(struct schema_history_compaction_attempts_row,
				     "history_compaction_attempts",
				     history_compaction_attempts_fields),
		 .foreign_keys = history_compaction_attempts_foreign_keys,
		 .foreign_key_count = NB_ARRAY_SIZE(history_compaction_attempts_foreign_keys)},
	[SCHEMA_SESSION_PLANS] = {NORM_MODEL_METADATA(struct schema_session_plans_row,
						      "session_plans", session_plans_fields),
				  .foreign_keys = session_plans_foreign_keys,
				  .foreign_key_count = NB_ARRAY_SIZE(session_plans_foreign_keys)},
	[SCHEMA_PERMISSION_GRANTS] = {NORM_MODEL_METADATA(struct schema_permission_grants_row,
							  "permission_grants",
							  permission_grants_fields)},
	[SCHEMA_SCHEDULED_TASKS] = {NORM_MODEL_METADATA(struct schema_scheduled_tasks_row,
							"scheduled_tasks", scheduled_tasks_fields),
				    .foreign_keys = scheduled_tasks_foreign_keys,
				    .foreign_key_count =
					    NB_ARRAY_SIZE(scheduled_tasks_foreign_keys)},
	[SCHEMA_NOTIFICATIONS] = {NORM_MODEL_METADATA(struct schema_notifications_row,
						      "notifications", notifications_fields),
				  .foreign_keys = notifications_foreign_keys,
				  .foreign_key_count = NB_ARRAY_SIZE(notifications_foreign_keys)},
	[SCHEMA_CREDIT_EVENTS] = {NORM_MODEL_METADATA(struct schema_credit_events_row,
						      "credit_events", credit_events_fields)},
	[SCHEMA_MEMORY_NOTES] = {NORM_MODEL_METADATA(struct schema_memory_notes_row, "memory_notes",
						     memory_notes_fields),
				 .foreign_keys = memory_notes_foreign_keys,
				 .foreign_key_count = NB_ARRAY_SIZE(memory_notes_foreign_keys),
				 NORM_CHECKS(memory_notes_checks)},
	[SCHEMA_MEMORY_SUMMARIES] = {NORM_MODEL_METADATA(struct schema_memory_summaries_row,
							 "memory_summaries",
							 memory_summaries_fields),
				     .foreign_keys = memory_summaries_foreign_keys,
				     .foreign_key_count =
					     NB_ARRAY_SIZE(memory_summaries_foreign_keys)},
};

struct schema_index_group {
	enum schema_table table;
	const struct norm_index *indexes;
	size_t count;
};
static const struct schema_index_group index_groups[] = {
	{SCHEMA_SESSIONS, sessions_indexes, NB_ARRAY_SIZE(sessions_indexes)},
	{SCHEMA_MESSAGES, messages_indexes, NB_ARRAY_SIZE(messages_indexes)},
	{SCHEMA_REACT_TRACES, react_traces_indexes, NB_ARRAY_SIZE(react_traces_indexes)},
	{SCHEMA_OUTPUTS, outputs_indexes, NB_ARRAY_SIZE(outputs_indexes)},
	{SCHEMA_MEMORY_FACTS, memory_facts_indexes, NB_ARRAY_SIZE(memory_facts_indexes)},
	{SCHEMA_MEMORY_EPISODES, memory_episodes_indexes, NB_ARRAY_SIZE(memory_episodes_indexes)},
	{SCHEMA_MEMORY_PROCEDURES, memory_procedures_indexes,
	 NB_ARRAY_SIZE(memory_procedures_indexes)},
	{SCHEMA_MEMORY_SCOPES, memory_scopes_indexes, NB_ARRAY_SIZE(memory_scopes_indexes)},
	{SCHEMA_MEMORY_PREFERENCES, memory_preferences_indexes,
	 NB_ARRAY_SIZE(memory_preferences_indexes)},
	{SCHEMA_MEMORY_CANDIDATES, memory_candidates_indexes,
	 NB_ARRAY_SIZE(memory_candidates_indexes)},
	{SCHEMA_SUB_AGENT_TASKS, sub_agent_tasks_indexes, NB_ARRAY_SIZE(sub_agent_tasks_indexes)},
	{SCHEMA_SUB_AGENT_EVENTS, sub_agent_events_indexes,
	 NB_ARRAY_SIZE(sub_agent_events_indexes)},
	{SCHEMA_AGENT_MAILBOX, agent_mailbox_indexes, NB_ARRAY_SIZE(agent_mailbox_indexes)},
	{SCHEMA_MODEL_HISTORY_ITEMS, model_history_items_indexes,
	 NB_ARRAY_SIZE(model_history_items_indexes)},
	{SCHEMA_PERMISSION_GRANTS, permission_grants_indexes,
	 NB_ARRAY_SIZE(permission_grants_indexes)},
	{SCHEMA_SCHEDULED_TASKS, scheduled_tasks_indexes, NB_ARRAY_SIZE(scheduled_tasks_indexes)},
	{SCHEMA_NOTIFICATIONS, notifications_indexes, NB_ARRAY_SIZE(notifications_indexes)},
	{SCHEMA_CREDIT_EVENTS, credit_events_indexes, NB_ARRAY_SIZE(credit_events_indexes)},
	{SCHEMA_MEMORY_NOTES, memory_notes_indexes, NB_ARRAY_SIZE(memory_notes_indexes)},
	{SCHEMA_HISTORY_COMPACTION_ATTEMPTS, history_compaction_attempts_indexes,
	 NB_ARRAY_SIZE(history_compaction_attempts_indexes)},
};

const struct norm_model *schema_model(enum schema_table table)
{
	return table >= 0 && table < SCHEMA_TABLE_COUNT ? &tables[table] : NULL;
}

size_t schema_field(const struct norm_model *model, const char *column)
{
	for (size_t i = 0; i < model->field_count; i++) {
		if (nb_str_eq_cstr(model->fields[i].column, column))
			return i;
	}

	return SIZE_MAX;
}

int schema_create_tables(struct orm_scope *scope)
{
	const struct norm_schema_options options = {.if_not_exists = true};
	struct norm_error error = {0};

	for (size_t i = 0; i < SCHEMA_TABLE_COUNT; i++) {
		int rc = orm_status(scope->database,
				    norm_schema_create(scope->connection, &tables[i], &options,
						       orm_scope_call(scope), &error),
				    &error);

		if (rc != NB_OK)
			return rc;
	}

	return NB_OK;
}

static bool schema_index_matches(const struct norm_table_metadata *metadata,
				 const struct norm_model *model, const struct norm_index *index)
{
	for (size_t candidate = 0; candidate < norm_table_metadata_index_count(metadata);
	     candidate++) {
		const struct norm_schema_index_column *first =
			norm_table_metadata_index_at(metadata, candidate);
		size_t count = 0;
		bool matches = true;

		if (!first->unique || first->partial || !first->is_key || first->position != 1)
			continue;

		for (size_t term = 0; term < norm_table_metadata_index_count(metadata); term++) {
			const struct norm_schema_index_column *column =
				norm_table_metadata_index_at(metadata, term);

			if (!nb_str_eq(column->name, first->name) || !column->is_key)
				continue;

			count++;
			if (column->position <= 0 ||
			    (uint64_t)column->position > index->field_count) {
				matches = false;
				continue;
			}

			size_t position = (size_t)column->position - 1;
			bool descending =
				index->directions && index->directions[position] == NORM_DESC;

			matches &= column->column_available && column->descending_available &&
				   column->descending == descending &&
				   (!column->collation_available || !column->collation.len ||
				    nb_str_eq_cstr(column->collation, "BINARY") ||
				    nb_str_eq_cstr(column->collation, "default")) &&
				   nb_str_eq(column->column,
					     model->fields[index->fields[position]].column);
		}

		if (matches && count == index->field_count)
			return true;
	}

	return false;
}

static void schema_metadata_cleanup(struct norm_table_metadata **owner)
{
	norm_table_metadata_free(*owner);
	*owner = NULL;
}

static int schema_indexes(struct orm_scope *scope, enum schema_table table)
{
	const struct norm_model *model = &tables[table];
	struct norm_table_metadata *metadata NB_CLEANUP(schema_metadata_cleanup) = NULL;
	struct norm_error error = {0};

	for (size_t group = 0; group < NB_ARRAY_SIZE(index_groups); group++) {
		if (index_groups[group].table != table)
			continue;

		int rc = orm_status(
			scope->database,
			norm_schema_read_table(scope->connection, NB_STR_NULL, model->table, NULL,
					       orm_scope_call(scope), &metadata, &error),
			&error);

		if (rc != NB_OK)
			return rc;

		for (size_t i = 0; i < index_groups[group].count; i++) {
			const struct norm_index *index = &index_groups[group].indexes[i];

			if (index->unique && !index->predicate &&
			    schema_index_matches(metadata, model, index))
				continue;

			rc = orm_status(scope->database,
					norm_index_create(scope->connection, model, index, true,
							  orm_scope_call(scope), &error),
					&error);

			if (rc != NB_OK)
				return rc;
		}
	}

	return NB_OK;
}

int schema_create_indexes(struct orm_scope *scope)
{
	for (size_t i = 0; i < SCHEMA_TABLE_COUNT; i++) {
		int rc = schema_indexes(scope, (enum schema_table)i);

		if (rc != NB_OK)
			return rc;
	}

	return NB_OK;
}

int schema_create_credit(struct orm_scope *scope)
{
	const struct norm_schema_options options = {.if_not_exists = true};
	struct norm_error error = {0};
	int rc = orm_status(scope->database,
			    norm_schema_create(scope->connection, &tables[SCHEMA_CREDIT_EVENTS],
					       &options, orm_scope_call(scope), &error),
			    &error);

	return rc ? rc : schema_indexes(scope, SCHEMA_CREDIT_EVENTS);
}

int schema_create_revision_triggers(struct orm_scope *scope)
{
	const enum schema_table owners[] = {SCHEMA_MEMORY_FACTS, SCHEMA_MEMORY_CANDIDATES,
					    SCHEMA_MEMORY_EPISODES, SCHEMA_MEMORY_PROCEDURES,
					    SCHEMA_MEMORY_NOTES};
	static const struct norm_trigger templates[] = {
		NORM_TRIGGER("", NORM_TRIGGER_AFTER, NORM_TRIGGER_INSERT,
			     NORM_TRIGGER_SQLITE_BODY(
				     "UPDATE memory_scopes SET content_revision=content_revision+1 "
				     "WHERE session_id=NEW.session_id;"),
			     NORM_TRIGGER_PG_BODY(
				     "BEGIN UPDATE memory_scopes SET "
				     "content_revision=content_revision+1 "
				     "WHERE session_id=NEW.session_id; RETURN NULL; END;")),
		NORM_TRIGGER("", NORM_TRIGGER_AFTER, NORM_TRIGGER_UPDATE,
			     NORM_TRIGGER_SQLITE_BODY(
				     "UPDATE memory_scopes SET content_revision=content_revision+1 "
				     "WHERE session_id=NEW.session_id;"),
			     NORM_TRIGGER_PG_BODY(
				     "BEGIN UPDATE memory_scopes SET "
				     "content_revision=content_revision+1 "
				     "WHERE session_id=NEW.session_id; RETURN NULL; END;")),
		NORM_TRIGGER("", NORM_TRIGGER_AFTER, NORM_TRIGGER_DELETE,
			     NORM_TRIGGER_SQLITE_BODY(
				     "UPDATE memory_scopes SET content_revision=content_revision+1 "
				     "WHERE session_id=OLD.session_id;"),
			     NORM_TRIGGER_PG_BODY(
				     "BEGIN UPDATE memory_scopes SET "
				     "content_revision=content_revision+1 "
				     "WHERE session_id=OLD.session_id; RETURN NULL; END;")),
	};
	struct norm_error error = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(owners); i++) {
		const struct norm_model *model = &tables[owners[i]];

		for (size_t event = 0; event < NB_ARRAY_SIZE(templates); event++) {
			nb_buf name NB_CLEANUP(nb_buf_cleanup) = {0};

			if (!nb_buf_printf(&name, "%.*s_revision_%zu", (int)model->table.len,
					   model->table.ptr, event))
				return NB_ERR_NOMEM;

			struct norm_trigger trigger = templates[event];

			trigger.name = nb_buf_str(&name);
			int rc =
				orm_status(scope->database,
					   norm_trigger_create(scope->connection, model, &trigger,
							       true, orm_scope_call(scope), &error),
					   &error);

			if (rc != NB_OK)
				return rc;
		}
	}

	return NB_OK;
}
