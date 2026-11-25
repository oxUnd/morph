#include "persistence/orm/checkpoint_store.h"
#include "persistence/orm/internal.h"
#include "persistence/orm/records.h"

#include "persistence/orm/error_migration_models.h"
#include "schema_models.h"
#include "session_models.h"
#include <norm/postgresql.h>
#include <ctype.h>
#include <nb/vec.h>
#include <nb/cleanup.h>
#include <nb/buf.h>
#include <nb/log.h>
#include "errors.h"
#include <nb/file.h>
#include <nb/random.h>
#include <nb/encoding.h>
#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHECKPOINT_PLAN_MAX_FILES 4096U

struct checkpoint_header_row {
	int64_t id;
	int64_t session_id;
	nb_str workspace_root;
	nb_str turn_id;
	nb_str prompt;
	int64_t created_at;
	int32_t code_active;
	int32_t conversation_active;
	nb_str reason;
};
static const struct norm_field checkpoint_header_fields[] = {
	NORM_FIELD(struct checkpoint_header_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_FIELD(struct checkpoint_header_row, session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct checkpoint_header_row, workspace_root, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct checkpoint_header_row, turn_id, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct checkpoint_header_row, prompt, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct checkpoint_header_row, created_at, NORM_TYPE_I64, 0),
	{.column = NB_STR_LIT("code_active"),
	 .type = NORM_TYPE_I32,
	 .offset = offsetof(struct checkpoint_header_row, code_active),
	 .size = sizeof(((struct checkpoint_header_row *)0)->code_active),
	 .null_offset = SIZE_MAX,
	 .default_sql = NB_STR_LIT("1")},
	{.column = NB_STR_LIT("conversation_active"),
	 .type = NORM_TYPE_I32,
	 .offset = offsetof(struct checkpoint_header_row, conversation_active),
	 .size = sizeof(((struct checkpoint_header_row *)0)->conversation_active),
	 .null_offset = SIZE_MAX,
	 .default_sql = NB_STR_LIT("1")},
	{.column = NB_STR_LIT("reason"),
	 .type = NORM_TYPE_TEXT,
	 .offset = offsetof(struct checkpoint_header_row, reason),
	 .size = sizeof(((struct checkpoint_header_row *)0)->reason),
	 .null_offset = SIZE_MAX,
	 .default_sql = NB_STR_LIT("''")},
};
static const size_t checkpoint_session_field = 1;
static const size_t checkpoint_session_reference = SESSION_ID;
static const struct norm_foreign_key checkpoint_header_foreign_keys[] = {
	{.name = NB_STR_LIT("checkpoint_session_fk"),
	 .fields = &checkpoint_session_field,
	 .field_count = 1,
	 .references = &session_model,
	 .reference_fields = &checkpoint_session_reference,
	 .on_delete = NORM_FK_CASCADE},
};
static const struct norm_index checkpoint_header_indexes[] = {
	NORM_INDEX("code_checkpoints_session", 1, 0),
};
static const struct norm_model checkpoint_header_model = NORM_MODEL_WITH(
	struct checkpoint_header_row, "code_checkpoints", checkpoint_header_fields,
	NORM_FOREIGN_KEYS(checkpoint_header_foreign_keys), NORM_INDEXES(checkpoint_header_indexes));

struct checkpoint_previous_row {
	int64_t checkpoint_id;
	int32_t after_exists;
	int64_t after_mode;
	struct norm_bytes after_data;
	int32_t before_exists;
	int64_t before_mode;
	struct norm_bytes before_data;
	int32_t sealed;
	bool after_mode_missing;
	bool after_missing;
	bool before_mode_missing;
	bool before_missing;
};
static const struct norm_field checkpoint_previous_fields[] = {
	NORM_FIELD(struct checkpoint_previous_row, checkpoint_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct checkpoint_previous_row, after_exists, NORM_TYPE_I32, 0),
	NORM_NULLABLE_FIELD(struct checkpoint_previous_row, after_mode, after_mode_missing,
			    NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct checkpoint_previous_row, after_data, after_missing,
			    NORM_TYPE_BLOB, 0),
	NORM_FIELD(struct checkpoint_previous_row, before_exists, NORM_TYPE_I32, 0),
	NORM_NULLABLE_FIELD(struct checkpoint_previous_row, before_mode, before_mode_missing,
			    NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct checkpoint_previous_row, before_data, before_missing,
			    NORM_TYPE_BLOB, 0),
	NORM_FIELD(struct checkpoint_previous_row, sealed, NORM_TYPE_I32, 0),
};
static const struct norm_model checkpoint_previous_model =
	NORM_MODEL(struct checkpoint_previous_row, "", checkpoint_previous_fields);

struct checkpoint_budget_row {
	int64_t total;
	int64_t count;
};
static const struct norm_field checkpoint_budget_fields[] = {
	NORM_FIELD(struct checkpoint_budget_row, total, NORM_TYPE_I64, 0),
	NORM_FIELD(struct checkpoint_budget_row, count, NORM_TYPE_I64, 0),
};
static const struct norm_model checkpoint_budget_model =
	NORM_MODEL(struct checkpoint_budget_row, "", checkpoint_budget_fields);

struct checkpoint_write_row {
	nb_str path;
	int64_t before_size;
	int64_t after_size;
	int32_t pending;
	int32_t owned;
};
static const struct norm_field checkpoint_write_fields[] = {
	NORM_FIELD(struct checkpoint_write_row, path, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct checkpoint_write_row, before_size, NORM_TYPE_I64, 0),
	NORM_FIELD(struct checkpoint_write_row, after_size, NORM_TYPE_I64, 0),
	NORM_FIELD(struct checkpoint_write_row, pending, NORM_TYPE_I32, 0),
	NORM_FIELD(struct checkpoint_write_row, owned, NORM_TYPE_I32, 0),
};
static const struct norm_model checkpoint_write_model =
	NORM_MODEL(struct checkpoint_write_row, "", checkpoint_write_fields);

struct checkpoint_file_row {
	nb_str path;
	int32_t before_exists;
	int64_t before_mode;
	struct norm_bytes before_data;
	int32_t after_exists;
	int64_t after_mode;
	struct norm_bytes after_data;
	int32_t sealed;
	int32_t pending;
	bool before_mode_missing;
	bool before_missing;
	bool after_mode_missing;
	bool after_missing;
};
static const struct norm_field checkpoint_file_fields[] = {
	NORM_FIELD(struct checkpoint_file_row, path, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct checkpoint_file_row, before_exists, NORM_TYPE_I32, 0),
	NORM_NULLABLE_FIELD(struct checkpoint_file_row, before_mode, before_mode_missing,
			    NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct checkpoint_file_row, before_data, before_missing, NORM_TYPE_BLOB,
			    0),
	NORM_FIELD(struct checkpoint_file_row, after_exists, NORM_TYPE_I32, 0),
	NORM_NULLABLE_FIELD(struct checkpoint_file_row, after_mode, after_mode_missing,
			    NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct checkpoint_file_row, after_data, after_missing, NORM_TYPE_BLOB,
			    0),
	NORM_FIELD(struct checkpoint_file_row, sealed, NORM_TYPE_I32, 0),
	NORM_FIELD(struct checkpoint_file_row, pending, NORM_TYPE_I32, 0),
};
static const struct norm_model checkpoint_file_model =
	NORM_MODEL(struct checkpoint_file_row, "", checkpoint_file_fields);

struct checkpoint_list_row {
	struct checkpoint_header_row info;
	int64_t files;
};
static const struct norm_field checkpoint_list_fields[] = {
	NORM_FIELD_NAMED(struct checkpoint_list_row, info.id, "id", NORM_TYPE_I64, 0),
	NORM_FIELD_NAMED(struct checkpoint_list_row, info.session_id, "session_id", NORM_TYPE_I64,
			 0),
	NORM_FIELD_NAMED(struct checkpoint_list_row, info.workspace_root, "workspace_root",
			 NORM_TYPE_TEXT, 0),
	NORM_FIELD_NAMED(struct checkpoint_list_row, info.turn_id, "turn_id", NORM_TYPE_TEXT, 0),
	NORM_FIELD_NAMED(struct checkpoint_list_row, info.prompt, "prompt", NORM_TYPE_TEXT, 0),
	NORM_FIELD_NAMED(struct checkpoint_list_row, info.created_at, "created_at", NORM_TYPE_I64,
			 0),
	NORM_FIELD_NAMED(struct checkpoint_list_row, info.code_active, "code_active", NORM_TYPE_I32,
			 0),
	NORM_FIELD_NAMED(struct checkpoint_list_row, info.conversation_active,
			 "conversation_active", NORM_TYPE_I32, 0),
	NORM_FIELD_NAMED(struct checkpoint_list_row, info.reason, "reason", NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct checkpoint_list_row, files, NORM_TYPE_I64, 0),
};
static const struct norm_model checkpoint_list_model =
	NORM_MODEL(struct checkpoint_list_row, "", checkpoint_list_fields);

struct checkpoint_table {
	enum schema_table table;
};

enum checkpoint_header_field {
	CP_ID,
	CP_SESSION_ID,
	CP_ROOT,
	CP_TURN_ID,
	CP_PROMPT,
	CP_CREATED_AT,
	CP_CODE_ACTIVE,
	CP_CONVERSATION_ACTIVE,
	CP_REASON,
};

enum checkpoint_stored_field {
	CF_CHECKPOINT_ID,
	CF_PATH,
	CF_BEFORE_EXISTS,
	CF_BEFORE_MODE,
	CF_BEFORE_DATA,
	CF_AFTER_EXISTS,
	CF_AFTER_MODE,
	CF_AFTER_DATA,
	CF_SEALED,
	CF_PENDING_TOKEN,
};

struct checkpoint_stored_row {
	int64_t checkpoint_id;
	nb_str path;
	int32_t before_exists;
	int64_t before_mode;
	struct norm_bytes before_data;
	bool before_missing;
	int32_t after_exists;
	int64_t after_mode;
	struct norm_bytes after_data;
	bool after_missing;
	int32_t sealed;
	nb_str pending_token;
	bool pending_missing;
};
static const struct norm_field checkpoint_stored_fields[] = {
	NORM_FIELD(struct checkpoint_stored_row, checkpoint_id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY),
	NORM_TEXT_FIELD(struct checkpoint_stored_row, path, SIZE_MAX, NORM_FIELD_PRIMARY_KEY),
	NORM_FIELD(struct checkpoint_stored_row, before_exists, NORM_TYPE_I32, 0),
	NORM_FIELD(struct checkpoint_stored_row, before_mode, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct checkpoint_stored_row, before_data, before_missing,
			    NORM_TYPE_BLOB, 0),
	NORM_FIELD(struct checkpoint_stored_row, after_exists, NORM_TYPE_I32, 0),
	NORM_FIELD(struct checkpoint_stored_row, after_mode, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct checkpoint_stored_row, after_data, after_missing, NORM_TYPE_BLOB,
			    0),
	{.column = NB_STR_LIT("sealed"),
	 .type = NORM_TYPE_I32,
	 .offset = offsetof(struct checkpoint_stored_row, sealed),
	 .size = sizeof(((struct checkpoint_stored_row *)0)->sealed),
	 .null_offset = SIZE_MAX,
	 .default_sql = NB_STR_LIT("0")},
	NORM_NULLABLE_FIELD(struct checkpoint_stored_row, pending_token, pending_missing,
			    NORM_TYPE_TEXT, 0),
};
static const size_t checkpoint_owner_field = CF_CHECKPOINT_ID;
static const size_t checkpoint_header_id = CP_ID;
static const struct norm_foreign_key checkpoint_file_foreign_keys[] = {
	{.name = NB_STR_LIT("checkpoint_files_owner_fk"),
	 .fields = &checkpoint_owner_field,
	 .field_count = 1,
	 .references = &checkpoint_header_model,
	 .reference_fields = &checkpoint_header_id,
	 .on_delete = NORM_FK_CASCADE},
};
static const struct norm_model checkpoint_stored_model =
	NORM_MODEL_WITH(struct checkpoint_stored_row, "code_checkpoint_files",
			checkpoint_stored_fields, NORM_FOREIGN_KEYS(checkpoint_file_foreign_keys));

static void checkpoint_metadata_cleanup(struct norm_table_metadata **owner)
{
	norm_table_metadata_free(*owner);
	*owner = NULL;
}

static int checkpoint_read_rows(struct db *db, const struct norm_query *query,
				struct norm_rows **rows)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc : orm_read(&scope, query, rows);
}

static int checkpoint_update(struct db *db, const struct norm_model *model,
			     const struct norm_assignment *changes, size_t count,
			     struct norm_expr where)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc : orm_update_rows(&scope, model, changes, count, where, NULL);
}

static int checkpoint_delete(struct db *db, const struct norm_model *model, struct norm_expr where)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc : orm_delete_rows(&scope, model, where, NULL);
}

static int checkpoint_has_table(struct orm_scope *scope, nb_str schema, nb_str table, bool *present)
{
	struct norm_table_metadata *metadata NB_CLEANUP(checkpoint_metadata_cleanup) = NULL;
	struct norm_error error = {0};
	int rc = norm_schema_read_table(scope->connection, schema, table, NULL,
					orm_scope_call(scope), &metadata, &error);

	*present = rc == NB_OK;
	return rc == NB_ERR_NOT_FOUND ? NB_OK : orm_status(scope->database, rc, &error);
}

static int checkpoint_tables(struct checkpoint_table *tables, size_t capacity)
{
	const struct checkpoint_table source[] = {
		{SCHEMA_MESSAGE_ATTACHMENTS},	  {SCHEMA_HISTORY_SUMMARY_SOURCES},
		{SCHEMA_HISTORY_COMPACTIONS},	  {SCHEMA_HISTORY_COMPACTION_ATTEMPTS},
		{SCHEMA_MODEL_REQUEST_SNAPSHOTS}, {SCHEMA_REACT_TRACES},
		{SCHEMA_SESSION_PLANS},		  {SCHEMA_SESSION_SKILL_ACTIVATIONS},
		{SCHEMA_MODEL_HISTORY_ITEMS},	  {SCHEMA_MESSAGES},
	};

	if (capacity < NB_ARRAY_SIZE(source))
		return NB_ERR_RANGE;

	memcpy(tables, source, sizeof(source));
	return (int)NB_ARRAY_SIZE(source);
}

struct checkpoint_dynamic_model {
	struct norm_table_metadata *metadata;
	nb_vec fields;
	nb_buf name;
	nb_buf index_name;
	nb_buf foreign_key_name;
	nb_vec defaults;
	struct norm_model model;
	struct norm_index index;
	struct norm_foreign_key foreign_key;
	size_t owner_field;
	size_t reference_field;
};

static void checkpoint_dynamic_cleanup(struct checkpoint_dynamic_model *value)
{
	norm_table_metadata_free(value->metadata);
	nb_vec_destroy(&value->fields);
	nb_buf_destroy(&value->name);
	nb_buf_destroy(&value->index_name);
	nb_buf_destroy(&value->foreign_key_name);
	for (size_t i = 0; i < value->defaults.len; i++)
		free(*(char **)nb_vec_at(&value->defaults, i));

	nb_vec_destroy(&value->defaults);
	memset(value, 0, sizeof(*value));
}

static int checkpoint_column_type(nb_str name, nb_str native, const struct norm_model *canonical,
				  enum norm_type *type)
{
	for (size_t i = 0; i < canonical->field_count; i++) {
		if (nb_str_eq(name, canonical->fields[i].column)) {
			*type = canonical->fields[i].type;
			return NB_OK;
		}
	}

	char *text NB_CLEANUP(nb_charp_cleanup) = orm_text_copy(native);

	if (!text)
		return NB_ERR_NOMEM;

	for (char *part = text; *part; part++)
		*part = (char)toupper((unsigned char)*part);

	if (strstr(text, "INT"))
		*type = NORM_TYPE_I64;
	else if (strstr(text, "CHAR") || strstr(text, "CLOB") || strstr(text, "TEXT"))
		*type = NORM_TYPE_TEXT;
	else if (!text[0] || strstr(text, "BLOB") || strcmp(text, "BYTEA") == 0)
		*type = NORM_TYPE_BLOB;
	else if (strcmp(text, "BOOLEAN") == 0 || strcmp(text, "BOOL") == 0)
		*type = NORM_TYPE_BOOL;
	else if (strstr(text, "REAL") || strstr(text, "FLOA") || strstr(text, "DOUB"))
		*type = NORM_TYPE_F64;
	else
		return NB_ERR_UNSUPPORTED;

	return NB_OK;
}

static size_t checkpoint_value_size(enum norm_type type)
{
	switch (type) {
	case NORM_TYPE_I32:
		return sizeof(int32_t);
	case NORM_TYPE_I64:
		return sizeof(int64_t);
	case NORM_TYPE_BOOL:
		return sizeof(bool);
	case NORM_TYPE_F64:
		return sizeof(double);
	case NORM_TYPE_TEXT:
		return sizeof(nb_str);
	case NORM_TYPE_BLOB:
		return sizeof(struct norm_bytes);
	default:
		return 0;
	}
}

static int checkpoint_dynamic_layout(struct checkpoint_dynamic_model *value)
{
	if (value->fields.len > SIZE_MAX / (sizeof(struct norm_value) + sizeof(bool)))
		return NB_ERR_RANGE;

	size_t values_size = value->fields.len * sizeof(struct norm_value);
	size_t size = values_size + value->fields.len * sizeof(bool);
	size_t alignment = _Alignof(struct norm_value);

	if (size > SIZE_MAX - (alignment - 1))
		return NB_ERR_RANGE;

	for (size_t i = 0; i < value->fields.len; i++) {
		struct norm_field *field = nb_vec_at(&value->fields, i);

		field->offset = i * sizeof(struct norm_value) + offsetof(struct norm_value, as);
		field->size = checkpoint_value_size(field->type);
		field->null_offset =
			field->flags & NORM_FIELD_NULLABLE ? values_size + i : SIZE_MAX;
	}

	value->model.size = (size + alignment - 1) / alignment * alignment;
	value->model.alignment = alignment;
	value->model.fields = (const struct norm_field *)value->fields.data;
	value->model.field_count = value->fields.len;
	return NB_OK;
}

static int checkpoint_live_model(struct orm_scope *scope, enum schema_table table,
				 struct checkpoint_dynamic_model *out)
{
	const struct norm_model *canonical = schema_model(table);
	struct norm_error error = {0};
	const struct norm_schema_read_options limits = {.max_items = SIZE_MAX,
							.max_bytes = SIZE_MAX};
	int rc = orm_status(scope->database,
			    norm_schema_read_table(scope->connection, NB_STR_NULL, canonical->table,
						   &limits, orm_scope_call(scope), &out->metadata,
						   &error),
			    &error);

	if (rc != NB_OK)
		return rc;

	nb_vec_init(&out->fields, sizeof(struct norm_field));
	nb_vec_init(&out->defaults, sizeof(char *));
	for (size_t i = 0; i < norm_table_metadata_column_count(out->metadata); i++) {
		const struct norm_schema_column_info *column =
			norm_table_metadata_column_at(out->metadata, i);

		if (column->generated)
			continue;

		struct norm_field field = {.column = column->name, .flags = NORM_FIELD_NULLABLE};

		if (column->has_default) {
			nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
			char *copy NB_CLEANUP(nb_charp_cleanup) = NULL;

			if (!column->default_is_sql)
				return NB_ERR_UNSUPPORTED;

			if (!nb_buf_printf(&text, "(%.*s)", (int)column->default_value.len,
					   column->default_value.ptr))
				return NB_ERR_NOMEM;

			copy = orm_text_copy(nb_buf_str(&text));
			if (!copy || !nb_vec_push_copy(&out->defaults, &copy))
				return NB_ERR_NOMEM;

			field.default_sql = nb_str_from_cstr(copy);
			copy = NULL;
		}
		rc = checkpoint_column_type(column->name, column->native_type, canonical,
					    &field.type);
		if (rc != NB_OK)
			return rc;

		if (!nb_vec_push_copy(&out->fields, &field))
			return NB_ERR_NOMEM;
	}

	out->model.table = canonical->table;
	return checkpoint_dynamic_layout(out);
}

static int checkpoint_shadow_model(const struct checkpoint_dynamic_model *source,
				   struct checkpoint_dynamic_model *out)
{
	if (!nb_buf_printf(&out->name, "checkpoint_rows_%.*s", (int)source->model.table.len,
			   source->model.table.ptr) ||
	    !nb_buf_printf(&out->index_name, "checkpoint_rows_%.*s_owner",
			   (int)source->model.table.len, source->model.table.ptr))
		return NB_ERR_NOMEM;

	nb_vec_init(&out->fields, sizeof(struct norm_field));
	const struct norm_field owner = {.column = NB_STR_LIT("checkpoint_id"),
					 .type = NORM_TYPE_I64};

	if (!nb_vec_push_copy(&out->fields, &owner))
		return NB_ERR_NOMEM;

	for (size_t i = 0; i < source->fields.len; i++) {
		if (!nb_vec_push_copy(&out->fields, nb_vec_at(&source->fields, i)))
			return NB_ERR_NOMEM;
	}

	out->model.table = nb_buf_str(&out->name);
	if (!nb_buf_printf(&out->foreign_key_name, "%.*s_fk", (int)out->model.table.len,
			   out->model.table.ptr))
		return NB_ERR_NOMEM;

	out->owner_field = 0;
	out->reference_field = CP_ID;
	out->index = (struct norm_index){.name = nb_buf_str(&out->index_name),
					 .fields = &out->owner_field,
					 .field_count = 1};
	out->foreign_key = (struct norm_foreign_key){.name = nb_buf_str(&out->foreign_key_name),
						     .fields = &out->owner_field,
						     .field_count = 1,
						     .references = &checkpoint_header_model,
						     .reference_fields = &out->reference_field,
						     .on_delete = NORM_FK_CASCADE};
	out->model.foreign_keys = &out->foreign_key;
	out->model.foreign_key_count = 1;
	out->model.indexes = &out->index;
	out->model.index_count = 1;
	return checkpoint_dynamic_layout(out);
}

struct checkpoint_session_filter {
	struct norm_table_ref parent;
	struct norm_query_field field;
	struct norm_query query;
	struct norm_term child;
	struct norm_expr value;
};

static void checkpoint_filter_init(struct checkpoint_session_filter *out,
				   const struct norm_model *model, enum schema_table table,
				   int64_t session_id, const struct norm_table_ref *source)
{
	if (table != SCHEMA_MESSAGE_ATTACHMENTS && table != SCHEMA_HISTORY_SUMMARY_SOURCES) {
		const size_t field = schema_field(model, "session_id");

		out->value =
			norm_eq(source ? norm_table_col(source, field) : norm_col(model, field),
				norm_i64(session_id));
		return;
	}

	const struct norm_model *parent = schema_model(
		table == SCHEMA_MESSAGE_ATTACHMENTS ? SCHEMA_MESSAGES : SCHEMA_MODEL_HISTORY_ITEMS);
	out->parent = norm_table(parent, NB_STR_LIT("snapshot_parent"));
	out->field = (struct norm_query_field){
		norm_term_column(norm_table_col(&out->parent, schema_field(parent, "id"))), 0};
	out->query = (struct norm_query){
		.from = &out->parent,
		.projection = &orm_integer_model,
		.fields = &out->field,
		.field_count = 1,
		.where = norm_eq(norm_table_col(&out->parent, schema_field(parent, "session_id")),
				 norm_i64(session_id))};
	const size_t field = schema_field(
		model, table == SCHEMA_MESSAGE_ATTACHMENTS ? "message_id" : "summary_item_id");

	out->child =
		norm_term_column(source ? norm_table_col(source, field) : norm_col(model, field));
	out->value = norm_in_query(&out->child, &out->query, false);
}

struct checkpoint_lock {
	struct orm_scope scope;
};

struct checkpoint_state {
	unsigned char *bytes;
	size_t size;
	mode_t mode;
	int exists;
};

struct checkpoint_file {
	char *path;
	struct checkpoint_state before;
	struct checkpoint_state after;
	struct checkpoint_state current;
	int sealed;
	int pending;
	int applied;
	int compensated;
	int conflict;
	char backup[80];
	char compensation_backup[80];
	char staging[80];
	char compensation_staging[80];
	int staging_owned;
	int compensation_staging_owned;
};

struct checkpoint_info {
	int64_t id;
	int64_t session_id;
	int64_t created_at;
	char *root;
	char *turn_id;
	char *prompt;
	char *reason;
	int code_active;
	int conversation_active;
};

struct checkpoint_plan {
	struct checkpoint_info info;
	nb_vec files;
	size_t bytes;
};

/* Delete in this order; restore in reverse order to satisfy foreign keys. */

static void checkpoint_fd_cleanup(int *fd)
{
	if (*fd >= 0)
		close(*fd);
	*fd = -1;
}

static void checkpoint_lock_cleanup(struct checkpoint_lock *lock)
{
	orm_scope_cleanup(&lock->scope);
}

static void checkpoint_cursor_cleanup(struct norm_cursor **owner)
{
	if (*owner)
		norm_cursor_close(owner, NULL);
}

static void checkpoint_state_cleanup(struct checkpoint_state *state)
{
	free(state->bytes);
	memset(state, 0, sizeof(*state));
}

static void checkpoint_info_cleanup(struct checkpoint_info *info)
{
	free(info->root);
	free(info->turn_id);
	free(info->prompt);
	free(info->reason);
	memset(info, 0, sizeof(*info));
}

static void checkpoint_plan_cleanup(struct checkpoint_plan *plan)
{
	struct checkpoint_file *file;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&plan->files) &&
			     (file = (struct checkpoint_file *)nb_vec_at(&plan->files, _i_));
	     _i_++) {
		free(file->path);
		checkpoint_state_cleanup(&file->before);
		checkpoint_state_cleanup(&file->after);
		checkpoint_state_cleanup(&file->current);
	}
	nb_vec_cleanup(&plan->files);
	checkpoint_info_cleanup(&plan->info);
}

static int checkpoint_transaction_begin(struct db *db)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);

	return rc != NB_OK ? rc : orm_savepoint_begin(&scope, "morph_checkpoint");
}

static bool checkpoint_transaction_active(struct db *db)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};

	return orm_scope_begin(db, &scope) == NB_OK &&
	       norm_tx_get_state(scope.connection) != NORM_TX_IDLE;
}

/* Pending rows retain the owner token of their editing attempt. Rotating the
 * application-owned token prevents later attempts from adopting stale writes. */
static int checkpoint_token_change(struct orm_scope *scope, bool seed)
{
	nb_buf token NB_CLEANUP(nb_buf_cleanup) = {0};

	if (seed && orm_checkpoint_token(scope->database))
		return NB_OK;

	int rc = nb_random_hex(&token, 16, NULL);

	return rc != NB_OK ? rc
			   : orm_checkpoint_set_token(scope->database, nb_buf_cstr(&token));
}

/* Snapshot tables retain old payload rows while live schemas gain columns. */
static int checkpoint_snapshot_upgrade(struct orm_scope *scope, enum schema_table table,
				       struct checkpoint_dynamic_model *snapshot)
{
	struct norm_table_metadata *stored NB_CLEANUP(checkpoint_metadata_cleanup) = NULL;
	struct norm_error error = {0};
	bool legacy_errors = false;
	int rc = orm_status(scope->database,
			    norm_schema_read_table(scope->connection, NB_STR_NULL,
						   snapshot->model.table, NULL,
						   orm_scope_call(scope), &stored, &error),
			    &error);

	for (size_t i = 1; rc == NB_OK && i < snapshot->model.field_count; i++) {
		bool found = false;
		const struct norm_field *field = &snapshot->model.fields[i];

		for (size_t column = 0; column < norm_table_metadata_column_count(stored);
		     column++) {
			if (nb_str_eq(field->column,
				      norm_table_metadata_column_at(stored, column)->name)) {
				found = true;
				break;
			}
		}

		if (!found) {
			rc = orm_status(scope->database,
					norm_schema_add_column(scope->connection, &snapshot->model,
							       i, orm_scope_call(scope), &error),
					&error);
			if (table == SCHEMA_HISTORY_COMPACTION_ATTEMPTS &&
			    nb_str_eq_cstr(field->column, "legacy_error_code"))
				legacy_errors = true;
		}
	}

	if (rc == NB_OK && legacy_errors) {
		size_t status_field = schema_field(&snapshot->model, "error_code");
		size_t legacy_field = schema_field(&snapshot->model, "legacy_error_code");
		const struct norm_term previous =
			norm_term_column(norm_col(&snapshot->model, status_field));
		struct error_migration_expression translated = {0};

		db_error_migration_expression(&translated, previous);
		const struct norm_assignment changes[] = {
			{.column = norm_col(&snapshot->model, legacy_field),
			 .expression = &previous},
			{.column = norm_col(&snapshot->model, status_field),
			 .expression = &translated.value},
		};
		const struct norm_update_query query = {.set = changes,
							.set_count = NB_ARRAY_SIZE(changes),
							.allow_all_rows = true};

		rc = orm_status(scope->database,
				norm_update(scope->connection, &snapshot->model, &query,
					    orm_scope_call(scope), NULL, &error),
				&error);
	}

	return rc;
}

static int checkpoint_schema_initialize(struct db *db)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_schema_options options = {.if_not_exists = true};
	const struct norm_model *const models[] = {
		&checkpoint_header_model, &checkpoint_stored_model};
	int rc = orm_scope_begin(db, &scope);

	for (size_t i = 0; rc == NB_OK && i < NB_ARRAY_SIZE(models); i++)
		rc = orm_status(
			db, norm_schema_create(scope.connection, models[i], &options,
					       orm_scope_call(&scope), &error),
			&error);

	if (rc == NB_OK)
		rc = checkpoint_token_change(&scope, true);

	struct checkpoint_table tables[10];
	int count = checkpoint_tables(tables, NB_ARRAY_SIZE(tables));

	for (int i = 0; rc == NB_OK && i < count; i++) {
		struct checkpoint_dynamic_model live NB_CLEANUP(checkpoint_dynamic_cleanup) = {0};
		struct checkpoint_dynamic_model shadow NB_CLEANUP(checkpoint_dynamic_cleanup) = {0};

		rc = checkpoint_live_model(&scope, tables[i].table, &live);
		if (rc == NB_ERR_NOT_FOUND) {
			rc = NB_OK;
			continue;
		}

		if (rc == NB_OK)
			rc = checkpoint_shadow_model(&live, &shadow);

		if (rc == NB_OK)
			rc = orm_status(db,
					norm_schema_create(scope.connection, &shadow.model,
							   &options, orm_scope_call(&scope),
							   &error),
					&error);

		if (rc == NB_OK)
			rc = checkpoint_snapshot_upgrade(&scope, tables[i].table, &shadow);
	}

	return rc;
}

static int checkpoint_transaction_finish(struct db *db, int status)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);

	return rc != NB_OK ? (status ? status : rc)
			   : orm_savepoint_finish(&scope, "morph_checkpoint", status);
}

static int checkpoint_transaction_rollback(struct db *db)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);

	return rc != NB_OK ? rc : orm_savepoint_abort(&scope, "morph_checkpoint");
}

static int checkpoint_schema(struct db *db)
{
	int rc = checkpoint_transaction_begin(db);

	if (rc != NB_OK)
		return rc;

	rc = checkpoint_transaction_finish(db, checkpoint_schema_initialize(db));
	if (rc == NB_OK)
		orm_checkpoint_set_ready(db, true);

	return rc;
}

static int checkpoint_enter(struct db *db, struct checkpoint_lock *lock)
{
	if (!db_is_open(db) || !lock)
		return NB_ERR_INVAL;

	int rc = orm_scope_begin(db, &lock->scope);

	if (rc != NB_OK)
		return rc;

	if (norm_tx_get_state(lock->scope.connection) != NORM_TX_IDLE)
		return NB_ERR_BUSY;

	return orm_checkpoint_ready(db) ? NB_OK : checkpoint_schema(db);
}

static int checkpoint_info_assign(const struct checkpoint_header_row *row,
				  struct checkpoint_info *out)
{
	out->id = row->id;
	out->session_id = row->session_id;
	out->root = orm_text_copy(row->workspace_root);
	out->turn_id = orm_text_copy(row->turn_id);
	out->prompt = orm_text_copy(row->prompt);
	out->reason = orm_text_copy(row->reason);
	out->created_at = row->created_at;
	out->code_active = row->code_active;
	out->conversation_active = row->conversation_active;
	return out->root && out->turn_id && out->prompt && out->reason ? NB_OK : NB_ERR_NOMEM;
}

static int checkpoint_info_read(struct db *db, int64_t session_id, int64_t id,
				struct checkpoint_info *out)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	const struct norm_table_ref checkpoints =
		norm_table(&checkpoint_header_model, NB_STR_LIT("c"));
	const struct norm_term now = norm_term_call(NORM_FUNCTION_CURRENT_EPOCH, NULL, 0);
	const struct norm_term age = norm_term_value(norm_i64(2592000));
	const struct norm_term cutoff = norm_term_binary(NORM_BINARY_SUBTRACT, &now, &age);
	const struct norm_term created =
		norm_term_column(norm_table_col(&checkpoints, CP_CREATED_AT));
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&checkpoints, CP_ID), norm_i64(id)),
		norm_compare_terms(&created, NORM_COMPARE_GE, &cutoff),
		norm_eq(norm_table_col(&checkpoints, CP_SESSION_ID), norm_i64(session_id)),
	};
	struct norm_query_field selected[NB_ARRAY_SIZE(checkpoint_header_fields)] = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(selected); i++)
		selected[i] = (struct norm_query_field){
			norm_term_column(norm_table_col(&checkpoints, i)), i};

	const struct norm_query query = {.from = &checkpoints,
					 .projection = &checkpoint_header_model,
					 .fields = selected,
					 .field_count = NB_ARRAY_SIZE(selected),
					 .where = norm_and(conditions, session_id ? 3 : 2)};
	int rc = checkpoint_read_rows(db, &query, &rows);

	if (rc != NB_OK)
		return rc;

	const struct checkpoint_header_row *row = norm_rows_at(rows, 0);

	if (!row)
		return NB_ERR_NOT_FOUND;

	return checkpoint_info_assign(row, out);
}

static int checkpoint_state_from_row(int32_t exists, int64_t mode, struct norm_bytes data,
				     struct checkpoint_state *out)
{
	if (data.len > CHECKPOINT_FILE_MAX_BYTES)
		return NB_ERR_TOO_BIG;

	out->exists = exists;
	out->mode = (mode_t)mode;
	out->size = data.len;
	if (data.len) {
		out->bytes = malloc(data.len);
		if (!out->bytes)
			return NB_ERR_NOMEM;

		memcpy(out->bytes, data.data, data.len);
	}

	return NB_OK;
}

static struct norm_value checkpoint_state_value(const struct checkpoint_state *state)
{
	return state->exists
		       ? norm_blob((struct norm_bytes){.data = state->bytes, .len = state->size})
		       : norm_null();
}

static int checkpoint_state_equal(const struct checkpoint_state *a,
				  const struct checkpoint_state *b)
{
	return a->exists == b->exists &&
	       (!a->exists || (a->mode == b->mode && a->size == b->size &&
			       (!a->size || !memcmp(a->bytes, b->bytes, a->size))));
}

/* Paths are normalized lexically before walking every directory through an fd. */
static int checkpoint_relative(const char *root, const char *path, char **out)
{
	if (!root || !path || !*path)
		return NB_ERR_INVAL;
	if (path[0] == '/') {
		/* Resolve only the workspace prefix. Resolving the whole candidate
		 * would hide symlinks below the workspace from the fd walk. */
		const char *relative = !strcmp(root, "/") ? path + 1 : NULL;
		size_t path_length = strlen(path);
		for (size_t length = 1; !relative && length <= path_length; length++) {
			if (path[length] && path[length] != '/')
				continue;
			char *prefix NB_CLEANUP(nb_charp_cleanup) = strndup(path, length);
			nb_buf resolved NB_CLEANUP(nb_buf_destroy) = {0};
			if (!prefix)
				return NB_ERR_NOMEM;
			int canonical_status = nb_path_canonical(&resolved, prefix, NULL);
			if (canonical_status == NB_ERR_NOMEM)
				return canonical_status;
			if (canonical_status == NB_OK && !strcmp(nb_buf_cstr(&resolved), root)) {
				relative = path + length;
				break;
			}
		}
		if (!relative)
			return NB_ERR_PERMISSION;
		path = relative;
	}
	char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(path);
	nb_buf normalized NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!copy)
		return NB_ERR_NOMEM;
	int rc = (nb_buf_init(&normalized),
		  nb_buf_reserve(&normalized, strlen(path) + 1) ? 0 : NB_ERR_NOMEM);
	char *save = NULL;
	for (char *part = strtok_r(copy, "/", &save); rc == 0 && part;
	     part = strtok_r(NULL, "/", &save)) {
		if (!strcmp(part, "."))
			continue;
		if (!strcmp(part, ".."))
			return NB_ERR_PERMISSION;
		if (normalized.len)
			rc = (nb_buf_append_char(&normalized, '/') ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_cstr(&normalized, part) ? 0 : NB_ERR_NOMEM);
	}
	if (rc != 0)
		return rc;
	if (!normalized.len || normalized.len >= PATH_MAX)
		return normalized.len ? NB_ERR_NAME_TOO_LONG : NB_ERR_INVAL;
	*out = (nb_buf_ok(&normalized) ? strdup(nb_buf_cstr(&normalized)) : NULL);
	return *out ? 0 : NB_ERR_NOMEM;
}

static int checkpoint_walk(const char *root, const char *relative, int *parent, char **leaf)
{
	char *full NB_CLEANUP(nb_charp_cleanup) = NULL;
	nb_buf path NB_CLEANUP(nb_buf_cleanup) = {0};
	int fd NB_CLEANUP(checkpoint_fd_cleanup) = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return nb_status_from_errno(errno, NULL);
	int rc = (nb_buf_init(&path),
		  nb_buf_reserve(&path, strlen(root) + strlen(relative) + 2) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_printf(&path, "%s/%s", root, relative) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	full = (nb_buf_ok(&path) ? strdup(nb_buf_cstr(&path)) : NULL);
	if (!full)
		return NB_ERR_NOMEM;
	char *save = NULL;
	char *part = strtok_r(full, "/", &save);
	while (part) {
		char *next = strtok_r(NULL, "/", &save);
		if (!next) {
			*leaf = strdup(part);
			if (!*leaf)
				return NB_ERR_NOMEM;
			*parent = fd;
			fd = -1;
			return 0;
		}
		int child = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (child < 0)
			return nb_status_from_errno(errno, NULL);
		close(fd);
		fd = child;
		part = next;
	}
	return NB_ERR_INVAL;
}

static int checkpoint_same_stat(const struct stat *a, const struct stat *b)
{
	if (a->st_dev != b->st_dev || a->st_ino != b->st_ino || a->st_size != b->st_size ||
	    a->st_mode != b->st_mode || a->st_nlink != b->st_nlink || a->st_mtime != b->st_mtime ||
	    a->st_ctime != b->st_ctime)
		return 0;
#ifdef __APPLE__
	return a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
	       a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
	return a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}

static int checkpoint_read_at(int parent, const char *leaf, size_t limit,
			      struct checkpoint_state *out)
{
	struct stat before;
	if (fstatat(parent, leaf, &before, AT_SYMLINK_NOFOLLOW) != 0)
		return errno == ENOENT ? 0 : nb_status_from_errno(errno, NULL);
	if (!S_ISREG(before.st_mode) || before.st_nlink != 1)
		return NB_ERR_LOOP;
	if (before.st_size < 0 || (uint64_t)before.st_size > CHECKPOINT_FILE_MAX_BYTES ||
	    (uint64_t)before.st_size > limit)
		return NB_ERR_TOO_BIG;
	int fd NB_CLEANUP(checkpoint_fd_cleanup) =
		openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return nb_status_from_errno(errno, NULL);
	struct stat opened;
	if (fstat(fd, &opened) != 0)
		return nb_status_from_errno(errno, NULL);
	if (!checkpoint_same_stat(&before, &opened) || !S_ISREG(opened.st_mode) ||
	    opened.st_nlink != 1)
		return NB_ERR_AGAIN;
	out->exists = 1;
	out->mode = opened.st_mode & 07777;
	out->size = (size_t)before.st_size;
	if (out->size) {
		out->bytes = malloc(out->size);
		if (!out->bytes)
			return NB_ERR_NOMEM;
	}
	size_t read_bytes = 0;
	while (read_bytes < out->size) {
		ssize_t got = read(fd, out->bytes + read_bytes, out->size - read_bytes);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0)
			return nb_status_from_errno(errno, NULL);
		if (got == 0)
			return NB_ERR_AGAIN;
		read_bytes += (size_t)got;
	}
	unsigned char extra;
	ssize_t extra_count;
	do {
		extra_count = read(fd, &extra, 1);
	} while (extra_count < 0 && errno == EINTR);
	if (extra_count < 0)
		return nb_status_from_errno(errno, NULL);
	if (extra_count)
		return NB_ERR_AGAIN;
	struct stat after;
	if (fstat(fd, &after) != 0)
		return nb_status_from_errno(errno, NULL);
	if (!checkpoint_same_stat(&after, &opened))
		return NB_ERR_AGAIN;
	if (fstatat(parent, leaf, &after, AT_SYMLINK_NOFOLLOW) != 0)
		return nb_status_from_errno(errno, NULL);
	if (!checkpoint_same_stat(&after, &opened))
		return NB_ERR_AGAIN;
	return 0;
}

static int checkpoint_read(const char *root, const char *path, size_t limit,
			   struct checkpoint_state *out)
{
	int parent NB_CLEANUP(checkpoint_fd_cleanup) = -1;
	char *leaf NB_CLEANUP(nb_charp_cleanup) = NULL;
	int rc = checkpoint_walk(root, path, &parent, &leaf);
	return rc == NB_ERR_NOT_FOUND ? 0
	       : rc != 0	      ? rc
				      : checkpoint_read_at(parent, leaf, limit, out);
}

static int checkpoint_current_size(const char *root, const char *path, size_t *size)
{
	int parent NB_CLEANUP(checkpoint_fd_cleanup) = -1;
	char *leaf NB_CLEANUP(nb_charp_cleanup) = NULL;
	struct stat info;
	int rc = checkpoint_walk(root, path, &parent, &leaf);

	*size = 0;
	if (rc == NB_ERR_NOT_FOUND)
		return 0;
	if (rc != 0)
		return rc;
	if (fstatat(parent, leaf, &info, AT_SYMLINK_NOFOLLOW) != 0)
		return errno == ENOENT ? 0 : nb_status_from_errno(errno, NULL);
	if (!S_ISREG(info.st_mode) || info.st_nlink != 1)
		return NB_ERR_LOOP;
	if (info.st_size < 0 || (uint64_t)info.st_size > CHECKPOINT_FILE_MAX_BYTES)
		return NB_ERR_TOO_BIG;
	*size = (size_t)info.st_size;
	return 0;
}

static int checkpoint_snapshot(struct db *db, int64_t id, int64_t session_id)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct checkpoint_table tables[10];
	int count = checkpoint_tables(tables, NB_ARRAY_SIZE(tables));
	int rc = orm_scope_begin(db, &scope);

	for (int index = 0; rc == NB_OK && index < count; index++) {
		struct checkpoint_dynamic_model live NB_CLEANUP(checkpoint_dynamic_cleanup) = {0};
		struct checkpoint_dynamic_model shadow NB_CLEANUP(checkpoint_dynamic_cleanup) = {0};
		nb_vec selected NB_CLEANUP(nb_vec_cleanup) = {0};

		rc = checkpoint_live_model(&scope, tables[index].table, &live);
		if (rc == NB_ERR_NOT_FOUND) {
			rc = NB_OK;
			continue;
		}

		if (rc == NB_OK)
			rc = checkpoint_shadow_model(&live, &shadow);

		if (rc != NB_OK)
			return rc;

		const struct norm_table_ref source =
			norm_table(&live.model, NB_STR_LIT("snapshot_source"));
		struct checkpoint_session_filter filter = {0};

		checkpoint_filter_init(&filter, &live.model, tables[index].table, session_id,
				       &source);
		nb_vec_init(&selected, sizeof(struct norm_query_field));
		const struct norm_query_field owner = {norm_term_value(norm_i64(id)), 0};

		if (!nb_vec_push_copy(&selected, &owner))
			return NB_ERR_NOMEM;

		for (size_t field = 0; field < live.model.field_count; field++) {
			const struct norm_query_field value = {
				norm_term_column(norm_table_col(&source, field)), field + 1};

			if (!nb_vec_push_copy(&selected, &value))
				return NB_ERR_NOMEM;
		}

		const struct norm_query query = {
			.from = &source,
			.projection = &shadow.model,
			.fields = (const struct norm_query_field *)selected.data,
			.field_count = selected.len,
			.where = filter.value};

		rc = orm_insert_from(&scope, &shadow.model, NULL, 0, &query, NULL, NULL);
	}

	return rc;
}

struct checkpoint_owner_row {
	int64_t checkpoint_id;
};
static const struct norm_field checkpoint_owner_fields[] = {
	NORM_FIELD(struct checkpoint_owner_row, checkpoint_id, NORM_TYPE_I64, 0),
};

static int checkpoint_mirror_owner(enum schema_table table, nb_buf *name, struct norm_model *out)
{
	const struct norm_model *source = schema_model(table);

	if (!nb_buf_printf(name, "checkpoint_rows_%.*s", (int)source->table.len, source->table.ptr))
		return NB_ERR_NOMEM;

	*out = (struct norm_model)NORM_MODEL(struct checkpoint_owner_row, "",
					     checkpoint_owner_fields);
	out->table = nb_buf_str(name);
	return NB_OK;
}

static int checkpoint_retention(struct db *db, int64_t session_id)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	const struct norm_table_ref recent =
		norm_table(&checkpoint_header_model, NB_STR_LIT("recent"));
	const struct norm_query_field id = {norm_term_column(norm_table_col(&recent, CP_ID)), 0};
	const struct norm_query_order order = {.value = id.value, .direction = NORM_DESC};
	const struct norm_query newest = {
		.from = &recent,
		.projection = &orm_integer_model,
		.fields = &id,
		.field_count = 1,
		.where = norm_eq(norm_table_col(&recent, CP_SESSION_ID), norm_i64(session_id)),
		.order = &order,
		.order_count = 1,
		.has_limit = true,
		.limit = 100};
	const struct norm_term current = norm_term_call(NORM_FUNCTION_CURRENT_EPOCH, NULL, 0);
	const struct norm_term age = norm_term_value(norm_i64(2592000));
	const struct norm_term cutoff = norm_term_binary(NORM_BINARY_SUBTRACT, &current, &age);
	const struct norm_term created =
		norm_term_column(norm_col(&checkpoint_header_model, CP_CREATED_AT));
	const struct norm_term checkpoint_id =
		norm_term_column(norm_col(&checkpoint_header_model, CP_ID));
	const struct norm_expr expired[] = {
		norm_compare_terms(&created, NORM_COMPARE_LT, &cutoff),
		norm_in_query(&checkpoint_id, &newest, true),
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&checkpoint_header_model, CP_SESSION_ID), norm_i64(session_id)),
		norm_or(expired, NB_ARRAY_SIZE(expired)),
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == NB_OK)
		rc = orm_delete_rows(&scope, &checkpoint_header_model,
				     norm_and(conditions, NB_ARRAY_SIZE(conditions)), NULL);

	const struct norm_query owners = {
		.from = &recent, .projection = &orm_integer_model, .fields = &id, .field_count = 1};
	const struct norm_term file_owner =
		norm_term_column(norm_col(&checkpoint_stored_model, CF_CHECKPOINT_ID));

	if (rc == NB_OK)
		rc = orm_delete_rows(&scope, &checkpoint_stored_model,
				     norm_in_query(&file_owner, &owners, true), NULL);

	struct checkpoint_table tables[10];
	int count = checkpoint_tables(tables, NB_ARRAY_SIZE(tables));

	for (int index = 0; rc == NB_OK && index < count; index++) {
		nb_buf name NB_CLEANUP(nb_buf_cleanup) = {0};
		struct norm_model mirror = {0};
		bool present = false;

		rc = checkpoint_mirror_owner(tables[index].table, &name, &mirror);
		if (rc == NB_OK)
			rc = checkpoint_has_table(&scope, NB_STR_NULL, mirror.table, &present);

		if (rc == NB_OK && present) {
			const struct norm_term owner = norm_term_column(norm_col(&mirror, 0));

			rc = orm_delete_rows(&scope, &mirror, norm_in_query(&owner, &owners, true),
					     NULL);
		}
	}

	return rc;
}

int checkpoint_store_create(struct db *db, int64_t session_id, const char *workdir,
			    const char *turn_id, const char *prompt, int64_t *id)
{
	struct checkpoint_lock lock NB_CLEANUP(checkpoint_lock_cleanup) = {0};
	nb_buf canonical NB_CLEANUP(nb_buf_cleanup) = {0};
	struct norm_error error = {0};
	struct norm_write_result result = {0};
	struct stat info;
	int rc;

	if (session_id <= 0 || !workdir || !*workdir || !id)
		return NB_ERR_INVAL;

	*id = 0;
	rc = nb_path_canonical(&canonical, workdir, NULL);
	if (rc != NB_OK)
		return rc;

	if (stat(nb_buf_cstr(&canonical), &info) != 0)
		return nb_status_from_errno(errno, NULL);

	if (!S_ISDIR(info.st_mode))
		return NB_ERR_NOT_DIR;

	rc = checkpoint_enter(db, &lock);
	if (rc == NB_OK)
		rc = checkpoint_transaction_begin(db);

	if (rc != NB_OK)
		return rc;

	const struct checkpoint_header_row row = {
		.session_id = session_id,
		.workspace_root = nb_buf_str(&canonical),
		.turn_id = nb_str_from_cstr(turn_id ? turn_id : ""),
		.prompt = nb_str_from_cstr(prompt ? prompt : ""),
		.created_at = (int64_t)time(NULL),
		.code_active = 1,
		.conversation_active = 1,
		.reason = NB_STR_LIT(""),
	};

	rc = orm_status(db,
			norm_insert(lock.scope.connection, &checkpoint_header_model, &row, NULL,
				    orm_scope_call(&lock.scope), &result, &error),
			&error);
	if (rc == NB_OK)
		rc = checkpoint_snapshot(db, result.insert_id, session_id);

	if (rc == NB_OK)
		rc = checkpoint_retention(db, session_id);

	rc = checkpoint_transaction_finish(db, rc);
	if (rc == NB_OK)
		*id = result.insert_id;

	return rc;
}

static int checkpoint_previous_read(struct db *db, const struct checkpoint_info *info, int64_t id,
				    const char *path, struct norm_rows **rows)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	const struct norm_table_ref files = norm_table(&checkpoint_stored_model, NB_STR_LIT("f"));
	const struct norm_table_ref checkpoints =
		norm_table(&checkpoint_header_model, NB_STR_LIT("c"));
	const struct norm_join join = {
		.kind = NORM_JOIN_INNER,
		.table = &checkpoints,
		.on = norm_columns_eq(norm_table_col(&checkpoints, CP_ID),
				      norm_table_col(&files, CF_CHECKPOINT_ID))};
	const size_t fields[] = {CF_CHECKPOINT_ID, CF_AFTER_EXISTS, CF_AFTER_MODE,  CF_AFTER_DATA,
				 CF_BEFORE_EXISTS, CF_BEFORE_MODE,  CF_BEFORE_DATA, CF_SEALED};
	struct norm_join_column selected[NB_ARRAY_SIZE(fields)] = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(fields); i++)
		selected[i] = (struct norm_join_column){norm_table_col(&files, fields[i]), i};

	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&checkpoints, CP_SESSION_ID), norm_i64(info->session_id)),
		norm_eq(norm_table_col(&checkpoints, CP_ROOT), orm_string(info->root)),
		norm_eq(norm_table_col(&checkpoints, CP_CODE_ACTIVE), norm_i32(1)),
		norm_le(norm_table_col(&checkpoints, CP_ID), norm_i64(id)),
		norm_eq(norm_table_col(&files, CF_PATH), orm_string(path)),
	};
	const struct norm_order order = {.column = norm_table_col(&checkpoints, CP_ID),
					 .direction = NORM_DESC};
	const struct norm_join_query query = {
		.from = &files,
		.joins = &join,
		.join_count = 1,
		.projection = &checkpoint_previous_model,
		.columns = selected,
		.column_count = NB_ARRAY_SIZE(selected),
		.select = {.where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
			   .order = &order,
			   .order_count = 1,
			   .has_limit = true,
			   .limit = 1}};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc : orm_join_rows(&scope, &query, rows);
}

static int checkpoint_budget_read(struct db *db, int64_t id, struct norm_rows **rows)
{
	const struct norm_table_ref files = norm_table(&checkpoint_stored_model, NB_STR_LIT("f"));
	const struct norm_term before = norm_term_column(norm_table_col(&files, CF_BEFORE_DATA));
	const struct norm_term after = norm_term_column(norm_table_col(&files, CF_AFTER_DATA));
	const struct norm_term before_arguments[] = {
		norm_term_call(NORM_FUNCTION_BYTE_LENGTH, &before, 1),
		norm_term_value(norm_i32(0))};
	const struct norm_term after_arguments[] = {
		norm_term_call(NORM_FUNCTION_BYTE_LENGTH, &after, 1), norm_term_value(norm_i32(0))};
	const struct norm_term before_size =
		norm_term_call(NORM_FUNCTION_COALESCE, before_arguments, 2);
	const struct norm_term after_size =
		norm_term_call(NORM_FUNCTION_COALESCE, after_arguments, 2);
	const struct norm_term size = norm_term_binary(NORM_BINARY_ADD, &before_size, &after_size);
	const struct norm_term total_arguments[] = {norm_term_call(NORM_FUNCTION_SUM, &size, 1),
						    norm_term_value(norm_i32(0))};
	const struct norm_query_field selected[] = {
		{norm_term_call(NORM_FUNCTION_COALESCE, total_arguments, 2), 0},
		{norm_term_call(NORM_FUNCTION_COUNT, NULL, 0), 1},
	};
	const struct norm_query query = {
		.from = &files,
		.projection = &checkpoint_budget_model,
		.fields = selected,
		.field_count = NB_ARRAY_SIZE(selected),
		.where = norm_eq(norm_table_col(&files, CF_CHECKPOINT_ID), norm_i64(id))};

	return checkpoint_read_rows(db, &query, rows);
}

static int checkpoint_invalidate_earlier(struct db *db, const struct checkpoint_info *info,
					 int64_t id)
{
	const struct norm_table_ref files = norm_table(&checkpoint_stored_model, NB_STR_LIT("f"));
	const struct norm_query_field one = {norm_term_value(norm_i32(1)), 0};
	const struct norm_query files_query = {
		.from = &files,
		.projection = &orm_integer_model,
		.fields = &one,
		.field_count = 1,
		.where = norm_columns_eq(norm_table_col(&files, CF_CHECKPOINT_ID),
					 norm_col(&checkpoint_header_model, CP_ID))};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&checkpoint_header_model, CP_SESSION_ID),
			norm_i64(info->session_id)),
		norm_eq(norm_col(&checkpoint_header_model, CP_ROOT), orm_string(info->root)),
		norm_lt(norm_col(&checkpoint_header_model, CP_ID), norm_i64(id)),
		norm_exists(&files_query),
	};
	const struct norm_assignment changes[] = {
		{.column = norm_col(&checkpoint_header_model, CP_CODE_ACTIVE),
		 .value = norm_i32(0)},
		{.column = norm_col(&checkpoint_header_model, CP_REASON),
		 .value = orm_string("External changes preceded a later checkpoint; older code "
				     "history is no longer safe.")},
	};

	return checkpoint_update(db, &checkpoint_header_model, changes, NB_ARRAY_SIZE(changes),
				 norm_and(conditions, NB_ARRAY_SIZE(conditions)));
}

static int checkpoint_capture_file(struct db *db, int64_t id, const char *path,
				   const struct checkpoint_state *state)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	const struct norm_term pending = norm_term_value(orm_string(orm_checkpoint_token(db)));
	const size_t fields[] = {CF_CHECKPOINT_ID, CF_PATH,	   CF_BEFORE_EXISTS,
				 CF_BEFORE_MODE,   CF_BEFORE_DATA, CF_AFTER_EXISTS,
				 CF_AFTER_MODE,	   CF_AFTER_DATA,  CF_PENDING_TOKEN};
	const struct norm_query_field selected[] = {
		{norm_term_value(norm_i64(id)), CF_CHECKPOINT_ID},
		{norm_term_value(orm_string(path)), CF_PATH},
		{norm_term_value(norm_i32(state->exists)), CF_BEFORE_EXISTS},
		{norm_term_value(norm_i64((int64_t)state->mode)), CF_BEFORE_MODE},
		{norm_term_value(checkpoint_state_value(state)), CF_BEFORE_DATA},
		{norm_term_value(norm_i32(state->exists)), CF_AFTER_EXISTS},
		{norm_term_value(norm_i64((int64_t)state->mode)), CF_AFTER_MODE},
		{norm_term_value(checkpoint_state_value(state)), CF_AFTER_DATA},
		{pending, CF_PENDING_TOKEN},
	};
	const size_t keys[] = {CF_CHECKPOINT_ID, CF_PATH};
	const struct norm_assignment change = {
		.column = norm_col(&checkpoint_stored_model, CF_PENDING_TOKEN),
		.expression = &pending};
	const struct norm_conflict conflict = {.action = NORM_CONFLICT_UPDATE,
					       .fields = keys,
					       .field_count = NB_ARRAY_SIZE(keys),
					       .set = &change,
					       .set_count = 1};
	const struct norm_query query = {.projection = &checkpoint_stored_model,
					 .fields = selected,
					 .field_count = NB_ARRAY_SIZE(selected)};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_insert_from(&scope, &checkpoint_stored_model, fields, NB_ARRAY_SIZE(fields),
				    &query, &conflict, NULL);
}

int checkpoint_store_capture(struct db *db, int64_t id, const char *path)
{
	struct checkpoint_lock lock NB_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_info info NB_CLEANUP(checkpoint_info_cleanup) = {0};
	struct checkpoint_state current NB_CLEANUP(checkpoint_state_cleanup) = {0};
	struct checkpoint_state expected NB_CLEANUP(checkpoint_state_cleanup) = {0};
	char *relative NB_CLEANUP(nb_charp_cleanup) = NULL;
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	int64_t previous = 0;
	int rc;

	if (id <= 0 || !path)
		return NB_ERR_INVAL;

	rc = checkpoint_enter(db, &lock);
	if (rc == NB_OK)
		rc = checkpoint_info_read(db, 0, id, &info);

	if (rc != NB_OK)
		return rc;

	if (!info.code_active)
		return NB_ERR_STALE;

	rc = checkpoint_relative(info.root, path, &relative);
	if (rc == NB_OK)
		rc = checkpoint_read(info.root, relative, CHECKPOINT_FILE_MAX_BYTES, &current);

	if (rc == NB_OK)
		rc = checkpoint_transaction_begin(db);

	if (rc != NB_OK)
		return rc;

	rc = checkpoint_previous_read(db, &info, id, relative, &rows);
	if (rc == NB_OK && norm_rows_count(rows)) {
		const struct checkpoint_previous_row *row = norm_rows_at(rows, 0);

		previous = row->checkpoint_id;
		rc = row->sealed ? checkpoint_state_from_row(row->after_exists, row->after_mode,
							     row->after_data, &expected)
				 : checkpoint_state_from_row(row->before_exists, row->before_mode,
							     row->before_data, &expected);
	}

	orm_rows_cleanup(&rows);
	if (rc == NB_OK && previous != id) {
		rc = checkpoint_budget_read(db, id, &rows);
		if (rc == NB_OK) {
			const struct checkpoint_budget_row *row = norm_rows_at(rows, 0);

			if (!row)
				rc = MORPH_ERR_DB;
			else if (row->total < 0 ||
				 (uint64_t)row->total + 2 * current.size >
					 CHECKPOINT_DATA_MAX_BYTES ||
				 row->count >= CHECKPOINT_PLAN_MAX_FILES)
				rc = NB_ERR_TOO_BIG;
		}
	}

	if (rc == NB_OK && previous && !checkpoint_state_equal(&current, &expected)) {
		if (previous == id) {
			rc = NB_ERR_AGAIN;
		} else {
			rc = checkpoint_invalidate_earlier(db, &info, id);
		}
	}

	if (rc == NB_OK)
		rc = checkpoint_capture_file(db, id, relative, &current);

	return checkpoint_transaction_finish(db, rc);
}

static int checkpoint_write_rows(struct db *db, int64_t id, struct norm_rows **rows)
{
	const struct norm_table_ref files = norm_table(&checkpoint_stored_model, NB_STR_LIT("f"));
	const struct norm_term before = norm_term_column(norm_table_col(&files, CF_BEFORE_DATA));
	const struct norm_term after = norm_term_column(norm_table_col(&files, CF_AFTER_DATA));
	const struct norm_term before_arguments[] = {
		norm_term_call(NORM_FUNCTION_BYTE_LENGTH, &before, 1),
		norm_term_value(norm_i32(0))};
	const struct norm_term after_arguments[] = {
		norm_term_call(NORM_FUNCTION_BYTE_LENGTH, &after, 1), norm_term_value(norm_i32(0))};
	const struct norm_term pending = norm_term_column(norm_table_col(&files, CF_PENDING_TOKEN));
	const struct norm_term current_token =
		norm_term_value(orm_string(orm_checkpoint_token(db)));
	const struct norm_term one = norm_term_value(norm_i32(1));
	const struct norm_term zero = norm_term_value(norm_i32(0));
	const struct norm_case pending_case = {
		.when = norm_is_not_null(norm_table_col(&files, CF_PENDING_TOKEN)), .then = &one};
	const struct norm_case owned_case = {
		.when = norm_compare_terms(&pending, NORM_COMPARE_EQ, &current_token),
		.then = &one};
	const struct norm_query_field selected[] = {
		{norm_term_column(norm_table_col(&files, CF_PATH)), 0},
		{norm_term_call(NORM_FUNCTION_COALESCE, before_arguments, 2), 1},
		{norm_term_call(NORM_FUNCTION_COALESCE, after_arguments, 2), 2},
		{norm_term_case(&pending_case, 1, &zero), 3},
		{norm_term_case(&owned_case, 1, &zero), 4},
	};
	const struct norm_query query = {
		.from = &files,
		.projection = &checkpoint_write_model,
		.fields = selected,
		.field_count = NB_ARRAY_SIZE(selected),
		.where = norm_eq(norm_table_col(&files, CF_CHECKPOINT_ID), norm_i64(id))};

	return checkpoint_read_rows(db, &query, rows);
}

static int checkpoint_pending_paths(struct db *db, int64_t id, struct norm_rows **rows)
{
	const struct norm_table_ref files = norm_table(&checkpoint_stored_model, NB_STR_LIT("f"));
	const struct norm_term pending = norm_term_column(norm_table_col(&files, CF_PENDING_TOKEN));
	const struct norm_term current = norm_term_value(orm_string(orm_checkpoint_token(db)));
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&files, CF_CHECKPOINT_ID), norm_i64(id)),
		norm_compare_terms(&pending, NORM_COMPARE_EQ, &current),
	};
	const struct norm_query_field path = {norm_term_column(norm_table_col(&files, CF_PATH)), 0};
	const struct norm_query query = {.from = &files,
					 .projection = &orm_text_model,
					 .fields = &path,
					 .field_count = 1,
					 .where = norm_and(conditions, NB_ARRAY_SIZE(conditions))};

	return checkpoint_read_rows(db, &query, rows);
}

static int checkpoint_seal_file(struct db *db, int64_t id, nb_str path,
				const struct checkpoint_state *state)
{
	const struct norm_assignment changes[] = {
		{.column = norm_col(&checkpoint_stored_model, CF_AFTER_EXISTS),
		 .value = norm_i32(state->exists)},
		{.column = norm_col(&checkpoint_stored_model, CF_AFTER_MODE),
		 .value = norm_i64((int64_t)state->mode)},
		{.column = norm_col(&checkpoint_stored_model, CF_AFTER_DATA),
		 .value = checkpoint_state_value(state)},
		{.column = norm_col(&checkpoint_stored_model, CF_SEALED), .value = norm_i32(1)},
		{.column = norm_col(&checkpoint_stored_model, CF_PENDING_TOKEN),
		 .value = norm_null()},
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&checkpoint_stored_model, CF_CHECKPOINT_ID), norm_i64(id)),
		norm_eq(norm_col(&checkpoint_stored_model, CF_PATH), norm_text(path)),
	};

	return checkpoint_update(db, &checkpoint_stored_model, changes, NB_ARRAY_SIZE(changes),
				 norm_and(conditions, NB_ARRAY_SIZE(conditions)));
}

static int checkpoint_remove_unchanged(struct db *db, int64_t id)
{
	const struct norm_term before =
		norm_term_column(norm_col(&checkpoint_stored_model, CF_BEFORE_DATA));
	const struct norm_term after =
		norm_term_column(norm_col(&checkpoint_stored_model, CF_AFTER_DATA));
	const struct norm_expr same_content[] = {
		norm_columns_eq(norm_col(&checkpoint_stored_model, CF_BEFORE_MODE),
				norm_col(&checkpoint_stored_model, CF_AFTER_MODE)),
		norm_compare_terms(&before, NORM_COMPARE_IS, &after),
	};
	const struct norm_expr same_state[] = {
		norm_eq(norm_col(&checkpoint_stored_model, CF_BEFORE_EXISTS), norm_i32(0)),
		norm_and(same_content, NB_ARRAY_SIZE(same_content)),
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&checkpoint_stored_model, CF_CHECKPOINT_ID), norm_i64(id)),
		norm_eq(norm_col(&checkpoint_stored_model, CF_SEALED), norm_i32(1)),
		norm_is_null(norm_col(&checkpoint_stored_model, CF_PENDING_TOKEN)),
		norm_columns_eq(norm_col(&checkpoint_stored_model, CF_BEFORE_EXISTS),
				norm_col(&checkpoint_stored_model, CF_AFTER_EXISTS)),
		norm_or(same_state, NB_ARRAY_SIZE(same_state)),
	};

	return checkpoint_delete(db, &checkpoint_stored_model,
				 norm_and(conditions, NB_ARRAY_SIZE(conditions)));
}

int checkpoint_store_validate_write(struct db *db, int64_t id, const char *path, size_t bytes,
				    const char *removed_path)
{
	struct checkpoint_lock lock NB_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_info info NB_CLEANUP(checkpoint_info_cleanup) = {0};
	char *relative NB_CLEANUP(nb_charp_cleanup) = NULL;
	char *removed NB_CLEANUP(nb_charp_cleanup) = NULL;
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	size_t total = 0;
	bool found = false;
	bool found_removed = !removed_path;
	int rc;

	if (id <= 0 || !path)
		return NB_ERR_INVAL;

	if (bytes > CHECKPOINT_FILE_MAX_BYTES)
		return NB_ERR_TOO_BIG;

	rc = checkpoint_enter(db, &lock);
	if (rc == NB_OK)
		rc = checkpoint_info_read(db, 0, id, &info);

	if (rc != NB_OK)
		return rc;

	if (!info.code_active)
		return NB_ERR_STALE;

	rc = checkpoint_relative(info.root, path, &relative);
	if (rc == NB_OK && removed_path)
		rc = checkpoint_relative(info.root, removed_path, &removed);

	if (rc != NB_OK)
		return rc;

	if (removed && !strcmp(relative, removed))
		found_removed = true;

	rc = checkpoint_write_rows(db, id, &rows);
	if (rc != NB_OK)
		return rc;

	for (size_t index = 0; index < norm_rows_count(rows); index++) {
		const struct checkpoint_write_row *row = norm_rows_at(rows, index);
		size_t after;

		if (row->before_size < 0 || row->after_size < 0)
			return MORPH_ERR_DB;

		if (nb_str_eq_cstr(row->path, relative)) {
			if (!row->owned)
				return NB_ERR_STALE;

			found = true;
			after = bytes;
		} else if (removed && nb_str_eq_cstr(row->path, removed)) {
			if (!row->owned)
				return NB_ERR_STALE;

			found_removed = true;
			after = 0;
		} else if (row->pending) {
			char *stored_path NB_CLEANUP(nb_charp_cleanup) = orm_text_copy(row->path);

			if (!stored_path)
				return NB_ERR_NOMEM;

			rc = checkpoint_current_size(info.root, stored_path, &after);
			if (rc != NB_OK)
				return rc;
		} else {
			after = (size_t)row->after_size;
		}

		if ((uint64_t)row->before_size > CHECKPOINT_DATA_MAX_BYTES - total)
			return NB_ERR_TOO_BIG;

		total += (size_t)row->before_size;
		if (after > CHECKPOINT_DATA_MAX_BYTES - total)
			return NB_ERR_TOO_BIG;

		total += after;
	}

	return found && found_removed ? NB_OK : NB_ERR_STALE;
}

static int checkpoint_validate_data_budget(struct db *db, int64_t id)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	int rc = checkpoint_budget_read(db, id, &rows);

	if (rc != NB_OK)
		return rc;

	const struct checkpoint_budget_row *row = norm_rows_at(rows, 0);

	if (!row)
		return MORPH_ERR_DB;

	return row->total < 0 || (uint64_t)row->total > CHECKPOINT_DATA_MAX_BYTES ||
			       row->count < 0 || (uint64_t)row->count > CHECKPOINT_PLAN_MAX_FILES
		       ? NB_ERR_TOO_BIG
		       : NB_OK;
}

int checkpoint_store_seal(struct db *db, int64_t id)
{
	struct checkpoint_lock lock NB_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_info info NB_CLEANUP(checkpoint_info_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	int rc;

	if (id <= 0)
		return NB_ERR_INVAL;

	rc = checkpoint_enter(db, &lock);
	if (rc == NB_OK)
		rc = checkpoint_info_read(db, 0, id, &info);

	if (rc != NB_OK)
		return rc;

	if (!info.code_active)
		return NB_ERR_STALE;

	rc = checkpoint_transaction_begin(db);
	if (rc != NB_OK)
		return rc;

	rc = checkpoint_pending_paths(db, id, &rows);
	for (size_t index = 0; rc == NB_OK && index < norm_rows_count(rows); index++) {
		const struct orm_text *row = norm_rows_at(rows, index);
		struct checkpoint_state state NB_CLEANUP(checkpoint_state_cleanup) = {0};

		char *stored_path NB_CLEANUP(nb_charp_cleanup) = orm_text_copy(row->value);

		if (!stored_path) {
			rc = NB_ERR_NOMEM;
			break;
		}

		rc = checkpoint_read(info.root, stored_path, CHECKPOINT_FILE_MAX_BYTES, &state);
		if (rc == NB_OK)
			rc = checkpoint_seal_file(db, id, row->value, &state);
	}

	if (rc == NB_OK)
		rc = checkpoint_remove_unchanged(db, id);

	if (rc == NB_OK)
		rc = checkpoint_validate_data_budget(db, id);

	rc = checkpoint_transaction_finish(db, rc);
	int rotate_rc = checkpoint_token_change(&lock.scope, false);

	if (rc != NB_OK) {
		const struct norm_assignment changes[] = {
			{.column = norm_col(&checkpoint_header_model, CP_CODE_ACTIVE),
			 .value = norm_i32(0)},
			{.column = norm_col(&checkpoint_header_model, CP_REASON),
			 .value = orm_string("An editing attempt could not be sealed; code "
					     "restoration is unsafe.")},
		};

		(void)checkpoint_update(
			db, &checkpoint_header_model, changes, NB_ARRAY_SIZE(changes),
			norm_eq(norm_col(&checkpoint_header_model, CP_ID), norm_i64(id)));
	}

	return rc != NB_OK ? rc : rotate_rc;
}

static int checkpoint_plan_load(struct db *db, int64_t session_id, int64_t id,
				struct checkpoint_plan *plan)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_cursor *cursor NB_CLEANUP(checkpoint_cursor_cleanup) = NULL;
	const struct norm_result_options limits = {
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
		.max_row_bytes = 2 * CHECKPOINT_FILE_MAX_BYTES + PATH_MAX +
				 sizeof(struct checkpoint_file_row),
	};
	struct norm_error error = {0};
	bool has_row = false;
	int rc = checkpoint_info_read(db, session_id, id, &plan->info);

	if (rc == NB_OK) {
		nb_vec_init(&plan->files, sizeof(struct checkpoint_file));
		if (!nb_vec_reserve(&plan->files, 8))
			rc = NB_ERR_NOMEM;
	}

	if (rc != NB_OK || !plan->info.code_active)
		return rc;

	const struct norm_table_ref files = norm_table(&checkpoint_stored_model, NB_STR_LIT("f"));
	const struct norm_table_ref checkpoints =
		norm_table(&checkpoint_header_model, NB_STR_LIT("c"));
	const struct norm_join join = {
		.kind = NORM_JOIN_INNER,
		.table = &checkpoints,
		.on = norm_columns_eq(norm_table_col(&checkpoints, CP_ID),
				      norm_table_col(&files, CF_CHECKPOINT_ID))};
	const struct norm_term one = norm_term_value(norm_i32(1));
	const struct norm_term zero = norm_term_value(norm_i32(0));
	const struct norm_case pending = {
		.when = norm_is_not_null(norm_table_col(&files, CF_PENDING_TOKEN)), .then = &one};
	const size_t fields[] = {CF_PATH,	  CF_BEFORE_EXISTS, CF_BEFORE_MODE, CF_BEFORE_DATA,
				 CF_AFTER_EXISTS, CF_AFTER_MODE,    CF_AFTER_DATA,  CF_SEALED};
	struct norm_query_field selected[9] = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(fields); i++)
		selected[i] = (struct norm_query_field){
			norm_term_column(norm_table_col(&files, fields[i])), i};

	selected[8] = (struct norm_query_field){norm_term_case(&pending, 1, &zero), 8};
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&checkpoints, CP_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_table_col(&checkpoints, CP_ROOT), orm_string(plan->info.root)),
		norm_ge(norm_table_col(&checkpoints, CP_ID), norm_i64(id)),
		norm_eq(norm_table_col(&checkpoints, CP_CODE_ACTIVE), norm_i32(1)),
	};
	const struct norm_query_order order[] = {
		{.value = norm_term_column(norm_table_col(&checkpoints, CP_ID))},
		{.value = norm_term_column(norm_table_col(&files, CF_PATH))},
	};
	const struct norm_query query = {.from = &files,
					 .joins = &join,
					 .join_count = 1,
					 .projection = &checkpoint_file_model,
					 .fields = selected,
					 .field_count = NB_ARRAY_SIZE(selected),
					 .where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
					 .order = order,
					 .order_count = NB_ARRAY_SIZE(order),
					 .result_options = &limits};

	rc = orm_scope_begin(db, &scope);
	if (rc != NB_OK)
		return rc;

	/* Stream repeated large file versions into the bounded merged restore plan. */
	rc = orm_status(db, norm_query_cursor_open(scope.connection, &query, orm_scope_call(&scope),
						  &cursor, &error),
			&error);
	if (rc != NB_OK)
		return rc;

	while ((rc = orm_status(db, norm_cursor_next(cursor, &has_row, &error), &error)) == NB_OK &&
	       has_row) {
		const struct checkpoint_file_row *row = norm_cursor_row(cursor);
		struct checkpoint_file *file = NULL;

		for (size_t item = 0; item < plan->files.len; item++) {
			struct checkpoint_file *candidate = nb_vec_at(&plan->files, item);

			if (nb_str_eq_cstr(row->path, candidate->path)) {
				file = candidate;
				break;
			}
		}

		if (!file) {
			if (plan->files.len >= CHECKPOINT_PLAN_MAX_FILES)
				return NB_ERR_TOO_BIG;

			file = nb_vec_push(&plan->files);
			if (!file)
				return NB_ERR_NOMEM;

			memset(file, 0, sizeof(*file));
			file->path = orm_text_copy(row->path);
			if (!file->path)
				return NB_ERR_NOMEM;

			if (row->before_data.len > CHECKPOINT_PLAN_MAX_BYTES - plan->bytes)
				return NB_ERR_TOO_BIG;

			rc = checkpoint_state_from_row(row->before_exists, row->before_mode,
						       row->before_data, &file->before);
			if (rc != NB_OK)
				return rc;

			plan->bytes += file->before.size;
		}

		plan->bytes -= file->after.size;
		checkpoint_state_cleanup(&file->after);
		if (row->after_data.len > CHECKPOINT_PLAN_MAX_BYTES - plan->bytes)
			return NB_ERR_TOO_BIG;

		rc = checkpoint_state_from_row(row->after_exists, row->after_mode, row->after_data,
					       &file->after);
		if (rc != NB_OK)
			return rc;

		plan->bytes += file->after.size;
		file->sealed = row->sealed;
		file->pending = row->pending;
	}

	if (rc != NB_OK)
		return rc;

	rc = orm_status(db, norm_cursor_close(&cursor, &error), &error);
	if (rc != NB_OK)
		return rc;

	for (size_t index = 0; index < plan->files.len;) {
		struct checkpoint_file *file = nb_vec_at(&plan->files, index);

		if (file->sealed && !file->pending &&
		    checkpoint_state_equal(&file->before, &file->after)) {
			plan->bytes -= file->before.size + file->after.size;
			free(file->path);
			checkpoint_state_cleanup(&file->before);
			checkpoint_state_cleanup(&file->after);
			nb_vec_remove(&plan->files, index);
		} else {
			index++;
		}
	}

	return NB_OK;
}

static int checkpoint_file_compare(const void *a, const void *b)
{
	const struct checkpoint_file *left = a;
	const struct checkpoint_file *right = b;
	return strcmp(left->path, right->path);
}

static int checkpoint_plan_check(struct checkpoint_plan *plan)
{
	if (plan->files.len > 1)
		qsort(((void *)plan->files.data), plan->files.len, sizeof(struct checkpoint_file),
		      checkpoint_file_compare);
	struct checkpoint_file *file;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&plan->files) &&
			     (file = (struct checkpoint_file *)nb_vec_at(&plan->files, _i_));
	     _i_++) {
		char *normalized NB_CLEANUP(nb_charp_cleanup) = NULL;
		int rc = checkpoint_relative(plan->info.root, file->path, &normalized);
		if (rc == 0 && strcmp(normalized, file->path))
			rc = NB_ERR_PERMISSION;
		if (rc == 0)
			rc = checkpoint_read(plan->info.root, file->path,
					     CHECKPOINT_PLAN_MAX_BYTES - plan->bytes,
					     &file->current);
		if (rc == NB_ERR_NOMEM)
			return rc;
		if (file->current.size > CHECKPOINT_PLAN_MAX_BYTES - plan->bytes)
			return NB_ERR_TOO_BIG;
		plan->bytes += file->current.size;
		file->conflict = rc;
		if (rc == 0 && (!file->sealed || file->pending))
			file->conflict = NB_ERR_IN_PROGRESS;
		if (rc == 0 && !file->conflict &&
		    !checkpoint_state_equal(&file->current, &file->after))
			file->conflict = NB_ERR_AGAIN;
	}
	return 0;
}

static int checkpoint_conflict_count(const struct checkpoint_plan *plan)
{
	int count = 0;
	for (size_t i = 0; i < plan->files.len; i++) {
		const struct checkpoint_file *file = nb_vec_at(&plan->files, i);
		count += file->conflict != 0;
	}
	return count;
}

static int checkpoint_json_info(cJSON *json, const struct checkpoint_info *info)
{
	return cJSON_AddNumberToObject(json, "id", (double)info->id) &&
	       cJSON_AddStringToObject(json, "turn_id", info->turn_id) &&
	       cJSON_AddStringToObject(json, "prompt", info->prompt) &&
	       cJSON_AddStringToObject(json, "workspace_root", info->root) &&
	       cJSON_AddNumberToObject(json, "created_at", (double)info->created_at) &&
	       cJSON_AddBoolToObject(json, "code_active", info->code_active) &&
	       cJSON_AddBoolToObject(json, "conversation_restorable", info->conversation_active) &&
	       cJSON_AddStringToObject(json, "reason", info->reason);
}

static const char *checkpoint_conflict_reason(int error)
{
	if (error == NB_ERR_IN_PROGRESS)
		return "An editing attempt was not sealed; its final file state is unknown.";
	if (error == NB_ERR_AGAIN)
		return "The file changed outside the captured editing history.";
	if (error == NB_ERR_LOOP || error == NB_ERR_NOT_DIR)
		return "A symlink, hardlink, or non-regular path prevents safe restoration.";
	if (error == NB_ERR_TOO_BIG)
		return "Checkpoint size limits exceeded (16 MiB/file, 128 MiB captured data, "
		       "256 MiB restore plan including compensation copies).";
	return nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, error);
}

static cJSON *checkpoint_report(const struct checkpoint_plan *plan)
{
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!report || !checkpoint_json_info(report, &plan->info))
		return NULL;
	cJSON *files = cJSON_AddArrayToObject(report, "files");
	cJSON *conflicts = cJSON_AddArrayToObject(report, "conflicts");
	if (!files || !conflicts ||
	    !cJSON_AddBoolToObject(report, "code_restorable",
				   plan->info.code_active && !checkpoint_conflict_count(plan)))
		return NULL;
	for (size_t i = 0; i < plan->files.len; i++) {
		const struct checkpoint_file *file = nb_vec_at(&plan->files, i);
		cJSON *row NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		if (!row || !cJSON_AddStringToObject(row, "path", file->path) ||
		    !cJSON_AddBoolToObject(row, "before_exists", file->before.exists) ||
		    !cJSON_AddBoolToObject(row, "restore_exists", file->before.exists) ||
		    !cJSON_AddBoolToObject(row, "after_exists", file->after.exists) ||
		    !cJSON_AddBoolToObject(row, "current_exists", file->current.exists) ||
		    !cJSON_AddNumberToObject(row, "before_bytes", (double)file->before.size) ||
		    !cJSON_AddNumberToObject(row, "current_bytes", (double)file->current.size) ||
		    !cJSON_AddNumberToObject(row, "restore_mode", (double)file->before.mode) ||
		    !cJSON_AddBoolToObject(row, "conflict", file->conflict != 0))
			return NULL;
		if ((cJSON_AddItemToArray(files, row) ? (row = NULL, NB_OK) : NB_ERR_NOMEM) != 0)
			return NULL;
		if (file->conflict) {
			row = cJSON_CreateObject();
			if (!row || !cJSON_AddStringToObject(row, "path", file->path) ||
			    !cJSON_AddStringToObject(row, "reason",
						     checkpoint_conflict_reason(file->conflict)) ||
			    !cJSON_AddNumberToObject(row, "error_code", file->conflict) ||
			    (cJSON_AddItemToArray(conflicts, row) ? (row = NULL, NB_OK)
								  : NB_ERR_NOMEM) != 0)
				return NULL;
		}
	}
	cJSON *value = report;
	report = NULL;
	return value;
}

int checkpoint_store_list_copy(struct db *db, int64_t session_id, cJSON **out)
{
	struct checkpoint_lock lock NB_CLEANUP(checkpoint_lock_cleanup) = {0};
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	struct norm_rows *records NB_CLEANUP(orm_rows_cleanup) = NULL;
	int rc;

	if (session_id <= 0 || !out)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	rc = checkpoint_enter(db, &lock);
	if (rc != NB_OK)
		return rc;

	report = cJSON_CreateObject();
	cJSON *rows = report ? cJSON_AddArrayToObject(report, "checkpoints") : NULL;

	if (!rows)
		return NB_ERR_NOMEM;

	const struct norm_table_ref checkpoints =
		norm_table(&checkpoint_header_model, NB_STR_LIT("c"));
	const struct norm_table_ref files = norm_table(&checkpoint_stored_model, NB_STR_LIT("f"));
	const struct norm_query_field count = {norm_term_call(NORM_FUNCTION_COUNT, NULL, 0), 0};
	const struct norm_query file_count = {
		.from = &files,
		.projection = &orm_integer_model,
		.fields = &count,
		.field_count = 1,
		.where = norm_columns_eq(norm_table_col(&files, CF_CHECKPOINT_ID),
					 norm_table_col(&checkpoints, CP_ID))};
	const struct norm_term now = norm_term_call(NORM_FUNCTION_CURRENT_EPOCH, NULL, 0);
	const struct norm_term age = norm_term_value(norm_i64(2592000));
	const struct norm_term cutoff = norm_term_binary(NORM_BINARY_SUBTRACT, &now, &age);
	const struct norm_term created =
		norm_term_column(norm_table_col(&checkpoints, CP_CREATED_AT));
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&checkpoints, CP_SESSION_ID), norm_i64(session_id)),
		norm_compare_terms(&created, NORM_COMPARE_GE, &cutoff),
	};
	struct norm_query_field selected[10] = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(checkpoint_header_fields); i++)
		selected[i] = (struct norm_query_field){
			norm_term_column(norm_table_col(&checkpoints, i)), i};

	selected[9] = (struct norm_query_field){norm_term_scalar(&file_count), 9};
	const struct norm_query_order order = {
		.value = norm_term_column(norm_table_col(&checkpoints, CP_ID)),
		.direction = NORM_DESC};
	const struct norm_query query = {.from = &checkpoints,
					 .projection = &checkpoint_list_model,
					 .fields = selected,
					 .field_count = NB_ARRAY_SIZE(selected),
					 .where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
					 .order = &order,
					 .order_count = 1};

	rc = checkpoint_read_rows(db, &query, &records);
	if (rc != NB_OK)
		return rc;

	for (size_t index = 0; index < norm_rows_count(records); index++) {
		const struct checkpoint_list_row *record = norm_rows_at(records, index);
		struct checkpoint_info info NB_CLEANUP(checkpoint_info_cleanup) = {0};
		cJSON *row NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

		rc = checkpoint_info_assign(&record->info, &info);
		if (rc != NB_OK)
			return rc;

		if (!row || !checkpoint_json_info(row, &info) ||
		    !cJSON_AddNumberToObject(row, "files", (double)record->files) ||
		    !cJSON_AddBoolToObject(row, "code_restorable", info.code_active) ||
		    !cJSON_AddItemToArray(rows, row))
			return NB_ERR_NOMEM;

		row = NULL;
	}

	*out = report;
	report = NULL;
	return NB_OK;
}

int checkpoint_store_preview_copy(struct db *db, int64_t session_id, int64_t id, cJSON **out)
{
	struct checkpoint_lock lock NB_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_plan plan NB_CLEANUP(checkpoint_plan_cleanup) = {0};
	if (session_id <= 0 || id <= 0 || !out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	int rc = checkpoint_enter(db, &lock);
	if (rc == 0)
		rc = checkpoint_plan_load(db, session_id, id, &plan);
	if (rc == 0)
		rc = checkpoint_plan_check(&plan);
	if (rc != 0)
		return rc;
	*out = checkpoint_report(&plan);
	return *out ? 0 : NB_ERR_NOMEM;
}

int checkpoint_store_info_copy(struct db *db, int64_t session_id, int64_t id, cJSON **out)
{
	struct checkpoint_lock lock NB_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_info info NB_CLEANUP(checkpoint_info_cleanup) = {0};
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	if (!out || session_id <= 0 || id <= 0)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	int rc = checkpoint_enter(db, &lock);
	if (rc == 0)
		rc = checkpoint_info_read(db, session_id, id, &info);
	if (rc != 0)
		return rc;
	report = cJSON_CreateObject();
	if (!report || !checkpoint_json_info(report, &info))
		return NB_ERR_NOMEM;
	*out = report;
	report = NULL;
	return 0;
}

static int checkpoint_write_bytes(int fd, const struct checkpoint_state *state)
{
	size_t written = 0;
	while (written < state->size) {
		ssize_t count = write(fd, state->bytes + written, state->size - written);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			return nb_status_from_errno(errno, NULL);
		if (!count)
			return NB_ERR_IO;
		written += (size_t)count;
	}
	if (fchmod(fd, state->mode) != 0 || fsync(fd) != 0)
		return nb_status_from_errno(errno, NULL);
	return 0;
}

static int checkpoint_replace_recover(int parent, const char *leaf, const char *backup,
				      const char *staging, int moved, int *changed,
				      int *staging_owned, int rc)
{
	if (moved && linkat(parent, backup, parent, leaf, 0) == 0) {
		if (unlinkat(parent, backup, 0) == 0)
			*changed = 0;
	}
	if (*staging_owned && unlinkat(parent, staging, 0) == 0)
		*staging_owned = 0;
	return rc;
}

/* Backups remain until the database commits. Publishing with linkat refuses
 * to replace a path created by a concurrent external writer. */
static int checkpoint_replace(const char *root, const char *path,
			      const struct checkpoint_state *expected,
			      const struct checkpoint_state *desired, const char *backup,
			      const char *staging, int *changed, int *staging_owned)
{
	*changed = 0;
	*staging_owned = 0;
	int parent NB_CLEANUP(checkpoint_fd_cleanup) = -1;
	int temporary NB_CLEANUP(checkpoint_fd_cleanup) = -1;
	char *leaf NB_CLEANUP(nb_charp_cleanup) = NULL;
	struct checkpoint_state current NB_CLEANUP(checkpoint_state_cleanup) = {0};
	int moved = 0;
	int rc = checkpoint_walk(root, path, &parent, &leaf);
	if (rc == NB_ERR_NOT_FOUND && !expected->exists && !desired->exists)
		return 0;
	if (rc != 0)
		return rc;
	rc = checkpoint_read_at(parent, leaf, CHECKPOINT_FILE_MAX_BYTES, &current);
	if (rc != 0)
		return rc;
	if (!checkpoint_state_equal(&current, expected))
		return NB_ERR_AGAIN;
	if (checkpoint_state_equal(expected, desired))
		return 0;
	if (desired->exists) {
		temporary = openat(parent, staging,
				   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (temporary < 0)
			return nb_status_from_errno(errno, NULL);
		*staging_owned = 1;
		rc = checkpoint_write_bytes(temporary, desired);
		checkpoint_fd_cleanup(&temporary);
		if (rc != 0)
			return checkpoint_replace_recover(parent, leaf, backup, staging, 0, changed,
							  staging_owned, rc);
	}
	if (expected->exists) {
		temporary = openat(parent, backup,
				   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (temporary < 0) {
			rc = nb_status_from_errno(errno, NULL);
			return checkpoint_replace_recover(parent, leaf, backup, staging, 0, changed,
							  staging_owned, rc);
		}
		checkpoint_fd_cleanup(&temporary);
		if (renameat(parent, leaf, parent, backup) != 0) {
			rc = nb_status_from_errno(errno, NULL);
			(void)unlinkat(parent, backup, 0);
			return checkpoint_replace_recover(parent, leaf, backup, staging, 0, changed,
							  staging_owned, rc);
		}
		moved = 1;
		*changed = 1;
		checkpoint_state_cleanup(&current);
		rc = checkpoint_read_at(parent, backup, CHECKPOINT_FILE_MAX_BYTES, &current);
		if (rc == 0 && !checkpoint_state_equal(&current, expected))
			rc = NB_ERR_AGAIN;
		if (rc != 0)
			return checkpoint_replace_recover(parent, leaf, backup, staging, moved,
							  changed, staging_owned, rc);
	}
	if (desired->exists && linkat(parent, staging, parent, leaf, 0) != 0) {
		rc = nb_status_from_errno(errno, NULL);
		return checkpoint_replace_recover(parent, leaf, backup, staging, moved, changed,
						  staging_owned, rc);
	}
	*changed = 1;
	if (*staging_owned) {
		if (unlinkat(parent, staging, 0) != 0)
			return nb_status_from_errno(errno, NULL);
		*staging_owned = 0;
	}
	return 0;
}

static int checkpoint_backup_remove(const char *root, const char *path, const char *backup)
{
	if (!backup[0])
		return 0;
	int parent NB_CLEANUP(checkpoint_fd_cleanup) = -1;
	char *leaf NB_CLEANUP(nb_charp_cleanup) = NULL;
	int rc = checkpoint_walk(root, path, &parent, &leaf);
	if (rc != 0)
		return rc;
	if (unlinkat(parent, backup, 0) != 0 && errno != ENOENT)
		return nb_status_from_errno(errno, NULL);
	return 0;
}

static int checkpoint_conversation_restore(struct db *db, const struct checkpoint_info *info)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct checkpoint_table tables[10];
	int count = checkpoint_tables(tables, NB_ARRAY_SIZE(tables));
	int rc = orm_scope_begin(db, &scope);

	for (int index = 0; rc == NB_OK && index < count; index++) {
		struct checkpoint_dynamic_model live NB_CLEANUP(checkpoint_dynamic_cleanup) = {0};
		struct checkpoint_session_filter filter = {0};

		rc = checkpoint_live_model(&scope, tables[index].table, &live);
		if (rc == NB_ERR_NOT_FOUND) {
			rc = NB_OK;
			continue;
		}

		if (rc == NB_OK) {
			checkpoint_filter_init(&filter, &live.model, tables[index].table,
					       info->session_id, NULL);
			rc = orm_delete_rows(&scope, &live.model, filter.value, NULL);
		}
	}

	for (int index = count - 1; rc == NB_OK && index >= 0; index--) {
		struct checkpoint_dynamic_model live NB_CLEANUP(checkpoint_dynamic_cleanup) = {0};
		struct checkpoint_dynamic_model shadow NB_CLEANUP(checkpoint_dynamic_cleanup) = {0};
		nb_vec selected NB_CLEANUP(nb_vec_cleanup) = {0};

		rc = checkpoint_live_model(&scope, tables[index].table, &live);
		if (rc == NB_ERR_NOT_FOUND) {
			rc = NB_OK;
			continue;
		}

		if (rc == NB_OK)
			rc = checkpoint_shadow_model(&live, &shadow);

		if (rc != NB_OK)
			return rc;

		const struct norm_table_ref source =
			norm_table(&shadow.model, NB_STR_LIT("snapshot_rows"));

		nb_vec_init(&selected, sizeof(struct norm_query_field));
		for (size_t field = 0; field < live.model.field_count; field++) {
			const struct norm_query_field value = {
				norm_term_column(norm_table_col(&source, field + 1)), field};

			if (!nb_vec_push_copy(&selected, &value))
				return NB_ERR_NOMEM;
		}

		const struct norm_query query = {
			.from = &source,
			.projection = &live.model,
			.fields = (const struct norm_query_field *)selected.data,
			.field_count = selected.len,
			.where = norm_eq(norm_table_col(&source, 0), norm_i64(info->id))};

		rc = orm_insert_from(&scope, &live.model, NULL, 0, &query, NULL, NULL);
	}

	if (rc != NB_OK || db_backend(db) != DB_BACKEND_POSTGRESQL)
		return rc;

	for (int index = 0; rc == NB_OK && index < count; index++) {
		const struct norm_model *model = schema_model(tables[index].table);
		struct norm_error error = {0};
		bool present = false;

		rc = checkpoint_has_table(&scope, NB_STR_NULL, model->table, &present);
		if (rc != NB_OK || !present)
			continue;

		for (size_t field = 0; rc == NB_OK && field < model->field_count; field++) {
			if (!(model->fields[field].flags & NORM_FIELD_GENERATED))
				continue;

			rc = orm_status(db,
					norm_postgresql_sync_identity(scope.connection, model,
								      field, orm_scope_call(&scope),
								      &error),
					&error);
		}
	}

	return rc;
}

static int checkpoint_mark_undone(struct db *db, const struct checkpoint_info *info,
				  enum checkpoint_restore_mode mode)
{
	const struct norm_table_ref files = norm_table(&checkpoint_stored_model, NB_STR_LIT("f"));
	const struct norm_query_field one = {norm_term_value(norm_i32(1)), 0};
	const struct norm_query file_query = {
		.from = &files,
		.projection = &orm_integer_model,
		.fields = &one,
		.field_count = 1,
		.where = norm_columns_eq(norm_table_col(&files, CF_CHECKPOINT_ID),
					 norm_col(&checkpoint_header_model, CP_ID))};
	const struct norm_term enabled = norm_term_value(norm_bool(true));
	const struct norm_term code_enabled =
		norm_term_value(norm_bool((mode & CHECKPOINT_RESTORE_CODE) != 0));
	const struct norm_term conversation_enabled =
		norm_term_value(norm_bool((mode & CHECKPOINT_RESTORE_CONVERSATION) != 0));
	const struct norm_expr code_conditions[] = {
		norm_compare_terms(&code_enabled, NORM_COMPARE_EQ, &enabled),
		norm_eq(norm_col(&checkpoint_header_model, CP_ROOT), orm_string(info->root)),
		norm_exists(&file_query),
	};
	const struct norm_expr code = norm_and(code_conditions, NB_ARRAY_SIZE(code_conditions));
	const struct norm_expr conversation =
		norm_compare_terms(&conversation_enabled, NORM_COMPARE_EQ, &enabled);
	const struct norm_term zero = norm_term_value(norm_i32(0));
	const struct norm_term prior_code =
		norm_term_column(norm_col(&checkpoint_header_model, CP_CODE_ACTIVE));
	const struct norm_term prior_conversation =
		norm_term_column(norm_col(&checkpoint_header_model, CP_CONVERSATION_ACTIVE));
	const struct norm_term prior_reason =
		norm_term_column(norm_col(&checkpoint_header_model, CP_REASON));
	const struct norm_term reason = norm_term_value(orm_string(
		"Code changes were rewound; this checkpoint belongs to an undone branch."));
	const struct norm_case code_case = {.when = code, .then = &zero};
	const struct norm_case conversation_case = {.when = conversation, .then = &zero};
	const struct norm_case reason_case = {.when = code, .then = &reason};
	const struct norm_term code_value = norm_term_case(&code_case, 1, &prior_code);
	const struct norm_term conversation_value =
		norm_term_case(&conversation_case, 1, &prior_conversation);
	const struct norm_term reason_value = norm_term_case(&reason_case, 1, &prior_reason);
	const struct norm_assignment changes[] = {
		{.column = norm_col(&checkpoint_header_model, CP_CODE_ACTIVE),
		 .expression = &code_value},
		{.column = norm_col(&checkpoint_header_model, CP_CONVERSATION_ACTIVE),
		 .expression = &conversation_value},
		{.column = norm_col(&checkpoint_header_model, CP_REASON),
		 .expression = &reason_value},
	};
	const struct norm_expr scope[] = {
		norm_eq(norm_col(&checkpoint_header_model, CP_ROOT), orm_string(info->root)),
		conversation,
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&checkpoint_header_model, CP_SESSION_ID),
			norm_i64(info->session_id)),
		norm_ge(norm_col(&checkpoint_header_model, CP_ID), norm_i64(info->id)),
		norm_or(scope, NB_ARRAY_SIZE(scope)),
	};

	return checkpoint_update(db, &checkpoint_header_model, changes, NB_ARRAY_SIZE(changes),
				 norm_and(conditions, NB_ARRAY_SIZE(conditions)));
}

static int checkpoint_effects_prepare(struct checkpoint_plan *plan, cJSON *report)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_2 = NB_OK;
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_3 = NB_OK;
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_4 = NB_OK;

	cJSON *effects = cJSON_AddArrayToObject(report, "effects");
	if (!effects || !cJSON_AddBoolToObject(report, "code_restored", 0) ||
	    !cJSON_AddBoolToObject(report, "conversation_restored", 0) ||
	    !cJSON_AddBoolToObject(report, "compensated", 0) ||
	    !cJSON_AddBoolToObject(report, "rollback_incomplete", 0) ||
	    !cJSON_AddBoolToObject(report, "database_rollback_failed", 0) ||
	    !cJSON_AddBoolToObject(report, "database_outcome_unknown", 0) ||
	    !cJSON_AddBoolToObject(report, "cleanup_incomplete", 0) ||
	    !cJSON_AddNumberToObject(report, "error_code", 0))
		return NB_ERR_NOMEM;
	struct checkpoint_file *file;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&plan->files) &&
			     (file = (struct checkpoint_file *)nb_vec_at(&plan->files, _i_));
	     _i_++) {
		int rc = ((file_status_1 =
				   ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
				    : !(nb_buf_clear(&file_value_1),
					nb_buf_append_cstr(&file_value_1, ".morph-rewind-save-"))
					    ? NB_ERR_NOMEM
					    : nb_random_hex(&file_value_1, 16, NULL))) != NB_OK
				  ? file_status_1
			  : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), file->backup,
							    sizeof(file->backup))) == NB_ERR_RANGE
				  ? NB_ERR_NO_SPACE
				  : file_status_1);
		if (rc == 0)
			rc = ((file_status_2 =
				       ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
					: !(nb_buf_clear(&file_value_2),
					    nb_buf_append_cstr(&file_value_2,
							       ".morph-rewind-undo-"))
						? NB_ERR_NOMEM
						: nb_random_hex(&file_value_2, 16, NULL))) != NB_OK
				      ? file_status_2
			      : (file_status_2 = nb_str_copy_to(
					 nb_buf_str(&file_value_2), file->compensation_backup,
					 sizeof(file->compensation_backup))) == NB_ERR_RANGE
				      ? NB_ERR_NO_SPACE
				      : file_status_2);
		if (rc == 0)
			rc = ((file_status_3 =
				       ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
					: !(nb_buf_clear(&file_value_3),
					    nb_buf_append_cstr(&file_value_3, ".morph-rewind-new-"))
						? NB_ERR_NOMEM
						: nb_random_hex(&file_value_3, 16, NULL))) != NB_OK
				      ? file_status_3
			      : (file_status_3 =
					 nb_str_copy_to(nb_buf_str(&file_value_3), file->staging,
							sizeof(file->staging))) == NB_ERR_RANGE
				      ? NB_ERR_NO_SPACE
				      : file_status_3);
		if (rc == 0)
			rc = ((file_status_4 =
				       ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
					: !(nb_buf_clear(&file_value_4),
					    nb_buf_append_cstr(&file_value_4,
							       ".morph-rewind-comp-"))
						? NB_ERR_NOMEM
						: nb_random_hex(&file_value_4, 16, NULL))) != NB_OK
				      ? file_status_4
			      : (file_status_4 = nb_str_copy_to(
					 nb_buf_str(&file_value_4), file->compensation_staging,
					 sizeof(file->compensation_staging))) == NB_ERR_RANGE
				      ? NB_ERR_NO_SPACE
				      : file_status_4);
		if (rc != 0)
			return rc;
		nb_buf backup_path NB_CLEANUP(nb_buf_cleanup) = {0};
		rc = (nb_buf_init(&backup_path),
		      nb_buf_reserve(&backup_path, strlen(file->path) + sizeof(file->backup))
			      ? 0
			      : NB_ERR_NOMEM);
		const char *slash = strrchr(file->path, '/');
		if (rc == 0 && slash)
			rc = (nb_buf_append(&backup_path, file->path,
					    (size_t)(slash - file->path) + 1)
				      ? 0
				      : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_cstr(&backup_path, file->backup) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
		cJSON *row NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		if (!row || !cJSON_AddStringToObject(row, "path", file->path) ||
		    !cJSON_AddStringToObject(row, "recovery_path", nb_buf_cstr(&backup_path)) ||
		    !cJSON_AddBoolToObject(row, "applied", 0) ||
		    !cJSON_AddBoolToObject(row, "compensated", 0) ||
		    !cJSON_AddBoolToObject(row, "recovery_available", 0) ||
		    !cJSON_AddStringToObject(row, "staging_basename", file->staging) ||
		    !cJSON_AddStringToObject(row, "compensation_staging_basename",
					     file->compensation_staging) ||
		    !cJSON_AddBoolToObject(row, "staging_remains", 0) ||
		    !cJSON_AddNumberToObject(row, "operation_error", 0) ||
		    !cJSON_AddNumberToObject(row, "cleanup_error", 0) ||
		    !cJSON_AddNumberToObject(row, "compensation_error", 0) ||
		    (cJSON_AddItemToArray(effects, row) ? (row = NULL, NB_OK) : NB_ERR_NOMEM) != 0)
			return NB_ERR_NOMEM;
	}
	return 0;
}

static void checkpoint_json_bool(cJSON *object, const char *name, int value)
{
	cJSON *field = cJSON_GetObjectItemCaseSensitive(object, name);
	if (field)
		cJSON_SetBoolValue(field, value);
}

static int checkpoint_compensate(struct checkpoint_plan *plan, cJSON *report)
{
	cJSON *effects = cJSON_GetObjectItemCaseSensitive(report, "effects");
	int failure = 0;
	for (size_t i = plan->files.len; i > 0; i--) {
		struct checkpoint_file *file = nb_vec_at(&plan->files, i - 1);
		cJSON *row = cJSON_GetArrayItem(effects, (int)(i - 1));
		if (file->staging_owned) {
			int cleanup = checkpoint_backup_remove(plan->info.root, file->path,
							       file->staging);
			if (cleanup) {
				failure = cleanup;
				checkpoint_json_bool(report, "cleanup_incomplete", 1);
				checkpoint_json_bool(row, "staging_remains", 1);
				cJSON_SetNumberValue(
					cJSON_GetObjectItemCaseSensitive(row, "cleanup_error"),
					cleanup);
			} else {
				file->staging_owned = 0;
			}
		}
		if (!file->applied)
			continue;
		int changed = 0;
		int rc = checkpoint_replace(plan->info.root, file->path, &file->before,
					    &file->current, file->compensation_backup,
					    file->compensation_staging, &changed,
					    &file->compensation_staging_owned);
		file->compensated = rc == 0;
		checkpoint_json_bool(row, "compensated", rc == 0);
		if (rc == 0) {
			int cleanup =
				checkpoint_backup_remove(plan->info.root, file->path, file->backup);
			int second = checkpoint_backup_remove(plan->info.root, file->path,
							      file->compensation_backup);
			if (cleanup || second) {
				int cleanup_error = cleanup ? cleanup : second;
				checkpoint_json_bool(report, "cleanup_incomplete", 1);
				checkpoint_json_bool(row, "recovery_available",
						     cleanup && file->current.exists);
				cJSON_SetNumberValue(
					cJSON_GetObjectItemCaseSensitive(row, "cleanup_error"),
					cleanup_error);
			}
		} else {
			failure = rc;
			checkpoint_json_bool(row, "recovery_available", file->current.exists);
			cJSON_SetNumberValue(
				cJSON_GetObjectItemCaseSensitive(row, "compensation_error"), rc);
		}
		if (file->compensation_staging_owned) {
			int cleanup = checkpoint_backup_remove(plan->info.root, file->path,
							       file->compensation_staging);
			if (cleanup) {
				failure = cleanup;
				checkpoint_json_bool(row, "staging_remains", 1);
				checkpoint_json_bool(report, "cleanup_incomplete", 1);
				cJSON_SetNumberValue(
					cJSON_GetObjectItemCaseSensitive(row, "cleanup_error"),
					cleanup);
			}
		}
	}
	checkpoint_json_bool(report, "compensated", !failure);
	checkpoint_json_bool(report, "rollback_incomplete", failure != 0);
	return failure;
}

int checkpoint_store_restore(struct db *db, int64_t session_id, int64_t id,
			     enum checkpoint_restore_mode mode, cJSON **out)
{
	struct checkpoint_lock lock NB_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_plan plan NB_CLEANUP(checkpoint_plan_cleanup) = {0};
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	if (!out || id <= 0 || session_id <= 0 || mode < CHECKPOINT_RESTORE_CODE ||
	    mode > CHECKPOINT_RESTORE_BOTH)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	int rc = checkpoint_enter(db, &lock);
	if (rc == 0) {
		rc = mode & CHECKPOINT_RESTORE_CODE
			     ? checkpoint_plan_load(db, session_id, id, &plan)
			     : checkpoint_info_read(db, session_id, id, &plan.info);
	}
	if (rc == 0 && (mode & CHECKPOINT_RESTORE_CODE))
		rc = checkpoint_plan_check(&plan);
	if (rc != 0)
		return rc;
	report = checkpoint_report(&plan);
	if (!report)
		return NB_ERR_NOMEM;
	rc = checkpoint_effects_prepare(&plan, report);
	if (rc != 0)
		return rc;
	if (((mode & CHECKPOINT_RESTORE_CODE) && !plan.info.code_active) ||
	    ((mode & CHECKPOINT_RESTORE_CONVERSATION) && !plan.info.conversation_active)) {
		rc = NB_ERR_STALE;
	} else if ((mode & CHECKPOINT_RESTORE_CODE) && checkpoint_conflict_count(&plan)) {
		rc = NB_ERR_AGAIN;
	} else {
		rc = checkpoint_transaction_begin(db);
		if (rc == 0) {
			if (mode & CHECKPOINT_RESTORE_CONVERSATION)
				rc = checkpoint_conversation_restore(db, &plan.info);
			cJSON *effects = cJSON_GetObjectItemCaseSensitive(report, "effects");
			for (size_t i = 0;
			     rc == 0 && (mode & CHECKPOINT_RESTORE_CODE) && i < plan.files.len;
			     i++) {
				struct checkpoint_file *file = nb_vec_at(&plan.files, i);
				rc = checkpoint_replace(plan.info.root, file->path, &file->after,
							&file->before, file->backup, file->staging,
							&file->applied, &file->staging_owned);
				cJSON *effect = cJSON_GetArrayItem(effects, (int)i);
				checkpoint_json_bool(effect, "applied", file->applied);
				cJSON_SetNumberValue(
					cJSON_GetObjectItemCaseSensitive(effect, "operation_error"),
					rc);
			}
			if (rc == 0)
				rc = checkpoint_mark_undone(db, &plan.info, mode);
			if (rc == 0) {
				rc = checkpoint_transaction_finish(db, NB_OK);
				if (rc == 0) {
					checkpoint_json_bool(report, "code_restored",
							     mode & CHECKPOINT_RESTORE_CODE);
					checkpoint_json_bool(
						report, "conversation_restored",
						mode & CHECKPOINT_RESTORE_CONVERSATION);
					for (size_t i = 0; i < plan.files.len; i++) {
						struct checkpoint_file *file =
							nb_vec_at(&plan.files, i);
						int cleanup = checkpoint_backup_remove(
							plan.info.root, file->path, file->backup);
						cJSON *effect = cJSON_GetArrayItem(effects, (int)i);
						cJSON_SetNumberValue(
							cJSON_GetObjectItemCaseSensitive(
								effect, "cleanup_error"),
							cleanup);
						if (cleanup) {
							checkpoint_json_bool(
								report, "cleanup_incomplete", 1);
							checkpoint_json_bool(effect,
									     "recovery_available",
									     file->current.exists);
						}
					}
				}
			}
			if (rc == MORPH_ERR_DB_OUTCOME_UNKNOWN) {
				/* The server may have committed. Keep restored files and their
				 * pre-restore backups until the database outcome is reconciled. */
				checkpoint_json_bool(report, "database_outcome_unknown", 1);
				for (size_t i = 0; i < plan.files.len; i++) {
					const struct checkpoint_file *file =
						nb_vec_at(&plan.files, i);
					cJSON *effect = cJSON_GetArrayItem(effects, (int)i);

					checkpoint_json_bool(effect, "recovery_available",
							     file->applied && file->current.exists);
				}
			} else if (rc != 0) {
				int compensation = checkpoint_compensate(&plan, report);
				int rollback = !checkpoint_transaction_active(db)
						       ? 0
						       : checkpoint_transaction_rollback(db);
				if (compensation || rollback) {
					checkpoint_json_bool(report, "database_rollback_failed",
							     rollback != 0);
					checkpoint_json_bool(report, "rollback_incomplete", 1);
					rc = NB_ERR_IO;
				}
			}
		}
	}
	cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(report, "error_code"), rc);
	*out = report;
	report = NULL;
	return rc;
}
