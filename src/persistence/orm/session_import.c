#include <norm/query.h>
#include "persistence/orm/session_models.h"
#include "persistence/orm/error_migration_models.h"
#include "persistence/orm/session_import.h"
#include "persistence/orm/internal.h"
#include "persistence/orm/records.h"
#include "persistence/orm/session.h"
#include "errors.h"
#include "blake3.h"
#include <nb/encoding.h>
#include <nb/file.h>
#include <nb/utf8.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Only durable conversation state is eligible for import. */
static const char *const history_tables[] = {
	"messages",
	"model_history_items",
	"react_traces",
	"outputs",
	"memory_profiles",
	"memory_facts",
	"memory_episodes",
	"memory_procedures",
	"history_compactions",
	"history_compaction_attempts",
	"sync_ui_messages",
	"message_attachments",
	"sub_agent_tasks",
	"sub_agent_events",
	"session_plans",
	"session_skill_activations",
	"agent_threads",
	"agent_mailbox",
	"agent_events",
};

/* Existing SQLite text values have no application length bound. */
struct import_schema_sync_import_receipts {
	nb_str origin;
	nb_str revision;
	int64_t session_id;
	bool missing_session_id;
};
static const struct norm_field import_schema_sync_import_receipts_fields[] = {
	{
		.column = NB_STR_LIT("origin"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct import_schema_sync_import_receipts, origin),
		.size = sizeof(((struct import_schema_sync_import_receipts *)0)->origin),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("revision"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct import_schema_sync_import_receipts, revision),
		.size = sizeof(((struct import_schema_sync_import_receipts *)0)->revision),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct import_schema_sync_import_receipts, session_id),
		.size = sizeof(((struct import_schema_sync_import_receipts *)0)->session_id),
		.null_offset =
			offsetof(struct import_schema_sync_import_receipts, missing_session_id),
		.flags = NORM_FIELD_NULLABLE,
	},
};
static const struct norm_model import_schema_sync_import_receipts_model = {
	.schema = NB_STR_LIT("main"),
	.table = NB_STR_LIT("sync_import_receipts"),
	.size = sizeof(struct import_schema_sync_import_receipts),
	.alignment = _Alignof(struct import_schema_sync_import_receipts),
	.fields = import_schema_sync_import_receipts_fields,
	.field_count = sizeof(import_schema_sync_import_receipts_fields) /
		       sizeof(import_schema_sync_import_receipts_fields[0]),
};

struct import_schema_sync_session_origins {
	int64_t session_id;
	nb_str origin;
};
static const struct norm_field import_schema_sync_session_origins_fields[] = {
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct import_schema_sync_session_origins, session_id),
		.size = sizeof(((struct import_schema_sync_session_origins *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("origin"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct import_schema_sync_session_origins, origin),
		.size = sizeof(((struct import_schema_sync_session_origins *)0)->origin),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};
static const struct norm_model import_schema_sync_session_origins_model = {
	.schema = NB_STR_LIT("main"),
	.table = NB_STR_LIT("sync_session_origins"),
	.size = sizeof(struct import_schema_sync_session_origins),
	.alignment = _Alignof(struct import_schema_sync_session_origins),
	.fields = import_schema_sync_session_origins_fields,
	.field_count = sizeof(import_schema_sync_session_origins_fields) /
		       sizeof(import_schema_sync_session_origins_fields[0]),
};

struct import_schema_sync_ui_messages {
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
static const struct norm_field import_schema_sync_ui_messages_fields[] = {
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct import_schema_sync_ui_messages, session_id),
		.size = sizeof(((struct import_schema_sync_ui_messages *)0)->session_id),
		.null_offset = offsetof(struct import_schema_sync_ui_messages, missing_session_id),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("turn_id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct import_schema_sync_ui_messages, turn_id),
		.size = sizeof(((struct import_schema_sync_ui_messages *)0)->turn_id),
		.null_offset = offsetof(struct import_schema_sync_ui_messages, missing_turn_id),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("core_message_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct import_schema_sync_ui_messages, core_message_id),
		.size = sizeof(((struct import_schema_sync_ui_messages *)0)->core_message_id),
		.null_offset =
			offsetof(struct import_schema_sync_ui_messages, missing_core_message_id),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("seq"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct import_schema_sync_ui_messages, seq),
		.size = sizeof(((struct import_schema_sync_ui_messages *)0)->seq),
		.null_offset = offsetof(struct import_schema_sync_ui_messages, missing_seq),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("type"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct import_schema_sync_ui_messages, type),
		.size = sizeof(((struct import_schema_sync_ui_messages *)0)->type),
		.null_offset = offsetof(struct import_schema_sync_ui_messages, missing_type),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("content"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct import_schema_sync_ui_messages, content),
		.size = sizeof(((struct import_schema_sync_ui_messages *)0)->content),
		.null_offset = offsetof(struct import_schema_sync_ui_messages, missing_content),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("attachments_json"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct import_schema_sync_ui_messages, attachments_json),
		.size = sizeof(((struct import_schema_sync_ui_messages *)0)->attachments_json),
		.null_offset =
			offsetof(struct import_schema_sync_ui_messages, missing_attachments_json),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("structured_data"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct import_schema_sync_ui_messages, structured_data),
		.size = sizeof(((struct import_schema_sync_ui_messages *)0)->structured_data),
		.null_offset =
			offsetof(struct import_schema_sync_ui_messages, missing_structured_data),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("agent_ui_ir"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct import_schema_sync_ui_messages, agent_ui_ir),
		.size = sizeof(((struct import_schema_sync_ui_messages *)0)->agent_ui_ir),
		.null_offset = offsetof(struct import_schema_sync_ui_messages, missing_agent_ui_ir),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("hitl_verdict"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct import_schema_sync_ui_messages, hitl_verdict),
		.size = sizeof(((struct import_schema_sync_ui_messages *)0)->hitl_verdict),
		.null_offset =
			offsetof(struct import_schema_sync_ui_messages, missing_hitl_verdict),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct import_schema_sync_ui_messages, created_at),
		.size = sizeof(((struct import_schema_sync_ui_messages *)0)->created_at),
		.null_offset = offsetof(struct import_schema_sync_ui_messages, missing_created_at),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("updated_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct import_schema_sync_ui_messages, updated_at),
		.size = sizeof(((struct import_schema_sync_ui_messages *)0)->updated_at),
		.null_offset = offsetof(struct import_schema_sync_ui_messages, missing_updated_at),
		.flags = NORM_FIELD_NULLABLE,
	},
};
static const struct norm_model import_schema_sync_ui_messages_model = {
	.schema = NB_STR_LIT("main"),
	.table = NB_STR_LIT("sync_ui_messages"),
	.size = sizeof(struct import_schema_sync_ui_messages),
	.alignment = _Alignof(struct import_schema_sync_ui_messages),
	.fields = import_schema_sync_ui_messages_fields,
	.field_count = sizeof(import_schema_sync_ui_messages_fields) /
		       sizeof(import_schema_sync_ui_messages_fields[0]),
};

struct import_schema_import_session_map {
	int64_t old_id;
	int64_t new_id;
	bool missing_new_id;
	int64_t is_new;
	bool missing_is_new;
};
static const struct norm_field import_schema_import_session_map_fields[] = {
	{
		.column = NB_STR_LIT("old_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct import_schema_import_session_map, old_id),
		.size = sizeof(((struct import_schema_import_session_map *)0)->old_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("new_id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct import_schema_import_session_map, new_id),
		.size = sizeof(((struct import_schema_import_session_map *)0)->new_id),
		.null_offset = offsetof(struct import_schema_import_session_map, missing_new_id),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("is_new"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct import_schema_import_session_map, is_new),
		.size = sizeof(((struct import_schema_import_session_map *)0)->is_new),
		.null_offset = offsetof(struct import_schema_import_session_map, missing_is_new),
		.flags = NORM_FIELD_NULLABLE,
		.default_sql = NB_STR_LIT("1"),
	},
};
static const struct norm_model import_schema_import_session_map_model = {
	.schema = NB_STR_LIT("temp"),
	.table = NB_STR_LIT("import_session_map"),
	.size = sizeof(struct import_schema_import_session_map),
	.alignment = _Alignof(struct import_schema_import_session_map),
	.fields = import_schema_import_session_map_fields,
	.field_count = sizeof(import_schema_import_session_map_fields) /
		       sizeof(import_schema_import_session_map_fields[0]),
};

static int import_schema(struct orm_scope *scope)
{
	static const struct norm_model *const models[] = {
		&import_schema_sync_import_receipts_model,
		&import_schema_sync_session_origins_model,
		&import_schema_sync_ui_messages_model,
		&import_schema_import_session_map_model,
	};
	const struct norm_schema_options options = {.if_not_exists = true};
	struct norm_error error = {0};
	int rc = 0;

	for (size_t i = 0; rc == 0 && i < sizeof(models) / sizeof(models[0]); i++)
		rc = orm_status(
			scope->database,
			norm_schema_create(scope->connection, models[i], &options, NULL, &error),
			&error);

	return rc;
}

static void import_cursor_cleanup(struct norm_cursor **owner)
{
	struct norm_error error = {0};

	if (*owner)
		norm_cursor_close(owner, &error);
}

static int import_has_column(struct orm_scope *scope, const char *schema, const char *table,
			     const char *column)
{
	bool present = false;

	return orm_table_has_column(scope, schema, table, column, &present) == 0 && present;
}

struct import_value_cell {
	union {
		int64_t integer;
		double real;
		nb_str text;
		struct norm_bytes bytes;
	} value;
	bool missing;
};

struct import_model {
	struct norm_model model;
	struct norm_field *fields;
	struct norm_table_metadata *metadata;
};

static void import_model_cleanup(struct import_model *owner)
{
	free(owner->fields);
	norm_table_metadata_free(owner->metadata);
	memset(owner, 0, sizeof(*owner));
}

static size_t import_field(const struct norm_model *model, const char *name)
{
	for (size_t i = 0; i < model->field_count; i++) {
		if (nb_str_eq_cstr(model->fields[i].column, name))
			return i;
	}

	return SIZE_MAX;
}

static enum norm_type import_native_type(nb_str native)
{
	if (nb_str_find(native, NB_STR_LIT("INT")) != SIZE_MAX)
		return NORM_TYPE_I64;

	if (nb_str_find(native, NB_STR_LIT("CHAR")) != SIZE_MAX ||
	    nb_str_find(native, NB_STR_LIT("CLOB")) != SIZE_MAX ||
	    nb_str_find(native, NB_STR_LIT("TEXT")) != SIZE_MAX)
		return NORM_TYPE_TEXT;

	if (nb_str_find(native, NB_STR_LIT("REAL")) != SIZE_MAX ||
	    nb_str_find(native, NB_STR_LIT("FLOA")) != SIZE_MAX ||
	    nb_str_find(native, NB_STR_LIT("DOUB")) != SIZE_MAX)
		return NORM_TYPE_F64;

	return NORM_TYPE_BLOB;
}

static int import_model_read(struct orm_scope *scope, const char *schema, const char *table,
			     struct import_model *out)
{
	struct norm_error error = {0};
	int rc = orm_status(scope->database,
			    norm_schema_read_table(scope->connection, nb_str_from_cstr(schema),
						   nb_str_from_cstr(table), NULL, NULL,
						   &out->metadata, &error),
			    &error);

	if (rc != 0)
		return rc;

	size_t count = norm_table_metadata_column_count(out->metadata);

	if (!count || count > SIZE_MAX / sizeof(*out->fields) ||
	    count > SIZE_MAX / sizeof(struct import_value_cell))
		return NB_ERR_RANGE;

	out->fields = calloc(count, sizeof(*out->fields));
	if (!out->fields)
		return NB_ERR_NOMEM;

	for (size_t i = 0; i < count; i++) {
		const struct norm_schema_column_info *column =
			norm_table_metadata_column_at(out->metadata, i);
		enum norm_type type = import_native_type(column->native_type);

		out->fields[i] = (struct norm_field){
			.column = column->name,
			.type = type,
			.offset = i * sizeof(struct import_value_cell),
			.size = type == NORM_TYPE_TEXT	 ? sizeof(nb_str)
				: type == NORM_TYPE_BLOB ? sizeof(struct norm_bytes)
							 : sizeof(int64_t),
			.null_offset = i * sizeof(struct import_value_cell) +
				       offsetof(struct import_value_cell, missing),
			.flags = NORM_FIELD_NULLABLE,
		};
	}

	out->model = (struct norm_model){
		.schema = nb_str_from_cstr(schema),
		.table = nb_str_from_cstr(table),
		.size = count * sizeof(struct import_value_cell),
		.alignment = _Alignof(struct import_value_cell),
		.fields = out->fields,
		.field_count = count,
	};
	return 0;
}

struct import_projection {
	struct norm_model model;
	struct norm_field *fields;
	struct norm_query_field *columns;
	struct norm_term *terms;
	size_t *targets;
};

static void import_projection_cleanup(struct import_projection *owner)
{
	free(owner->fields);
	free(owner->columns);
	free(owner->terms);
	free(owner->targets);
	memset(owner, 0, sizeof(*owner));
}

static int import_projection_init(struct import_projection *out, size_t count)
{
	if (!count || count > SIZE_MAX / (4 * sizeof(*out->terms)))
		return NB_ERR_RANGE;

	out->fields = calloc(count, sizeof(*out->fields));
	out->columns = calloc(count, sizeof(*out->columns));
	out->terms = calloc(count * 4, sizeof(*out->terms));
	out->targets = calloc(count, sizeof(*out->targets));
	if (!out->fields || !out->columns || !out->terms || !out->targets)
		return NB_ERR_NOMEM;

	out->model = (struct norm_model){
		.size = count * sizeof(struct import_value_cell),
		.alignment = _Alignof(struct import_value_cell),
		.fields = out->fields,
		.field_count = count,
	};
	return 0;
}

static void import_projection_field(struct import_projection *out, size_t index,
				    const struct norm_field *field, size_t target,
				    struct norm_term value)
{
	out->fields[index] = *field;
	out->fields[index].offset = index * sizeof(struct import_value_cell);
	out->fields[index].null_offset = index * sizeof(struct import_value_cell) +
					 offsetof(struct import_value_cell, missing);
	out->fields[index].flags = NORM_FIELD_NULLABLE;
	out->columns[index] = (struct norm_query_field){.value = value, .into = index};
	out->targets[index] = target;
}

struct revision_cell {
	nb_str kind;
	struct norm_bytes value;
	bool missing;
};

struct revision_projection {
	struct norm_model model;
	struct norm_field *fields;
	char (*names)[48];
};

static void revision_projection_cleanup(struct revision_projection *projection)
{
	free(projection->fields);
	free(projection->names);
	memset(projection, 0, sizeof(*projection));
}

static int revision_projection_init(struct revision_projection *projection, size_t count)
{
	if (!count || count > SIZE_MAX / (2 * sizeof(*projection->fields)) ||
	    count > SIZE_MAX / sizeof(struct revision_cell) ||
	    count > SIZE_MAX / (2 * sizeof(*projection->names)))
		return NB_ERR_RANGE;

	projection->fields = calloc(count * 2, sizeof(*projection->fields));
	projection->names = calloc(count * 2, sizeof(*projection->names));
	if (!projection->fields || !projection->names)
		return NB_ERR_NOMEM;

	projection->model = (struct norm_model){
		.size = count * sizeof(struct revision_cell),
		.alignment = _Alignof(struct revision_cell),
		.fields = projection->fields,
		.field_count = count * 2,
	};
	for (size_t i = 0; i < count; i++) {
		int first = snprintf(projection->names[2 * i], sizeof(projection->names[2 * i]),
				     "kind_%zu", i);
		int second = snprintf(projection->names[2 * i + 1],
				      sizeof(projection->names[2 * i + 1]), "value_%zu", i);

		if (first < 0 || second < 0 || (size_t)first >= sizeof(projection->names[2 * i]) ||
		    (size_t)second >= sizeof(projection->names[2 * i + 1]))
			return NB_ERR_RANGE;

		projection->fields[2 * i] = (struct norm_field){
			.column = nb_str_from_cstr(projection->names[2 * i]),
			.type = NORM_TYPE_TEXT,
			.max_length = SIZE_MAX,
			.offset = i * sizeof(struct revision_cell) +
				  offsetof(struct revision_cell, kind),
			.size = sizeof(((struct revision_cell *)0)->kind),
			.null_offset = SIZE_MAX,
		};
		projection->fields[2 * i + 1] = (struct norm_field){
			.column = nb_str_from_cstr(projection->names[2 * i + 1]),
			.type = NORM_TYPE_BLOB,
			.offset = i * sizeof(struct revision_cell) +
				  offsetof(struct revision_cell, value),
			.size = sizeof(((struct revision_cell *)0)->value),
			.null_offset = i * sizeof(struct revision_cell) +
				       offsetof(struct revision_cell, missing),
			.flags = NORM_FIELD_NULLABLE,
		};
	}

	return 0;
}

static bool revision_ignores(nb_str name)
{
	static const char *const ignored[] = {
		"id",
		"session_id",
		"core_message_id",
		"message_id",
		"summary_item_id",
		"parent_session_id",
		"child_session_id",
		"created_at",
		"updated_at",
	};

	for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++) {
		if (nb_str_eq_cstr(name, ignored[i]))
			return true;
	}

	return false;
}

static int revision_table(struct orm_scope *scope, const char *table, int64_t session_id,
			  blake3_hasher *hasher)
{
	struct import_model source NB_CLEANUP(import_model_cleanup) = {0};
	struct import_model auxiliary NB_CLEANUP(import_model_cleanup) = {0};
	struct revision_projection projection NB_CLEANUP(revision_projection_cleanup) = {0};
	struct import_projection query_fields NB_CLEANUP(import_projection_cleanup) = {0};
	struct norm_cursor *cursor NB_CLEANUP(import_cursor_cleanup) = NULL;
	nb_vec names NB_CLEANUP(nb_vec_cleanup) = {0};
	struct norm_error error = {0};
	bool attachments = strcmp(table, "message_attachments") == 0;
	bool tasks = strcmp(table, "sub_agent_tasks") == 0;
	bool events = strcmp(table, "sub_agent_events") == 0;
	bool agents = strcmp(table, "agent_threads") == 0 || strcmp(table, "agent_mailbox") == 0 ||
		      strcmp(table, "agent_events") == 0;
	const char *key = agents	? "root_session_id"
			  : attachments ? "message_id"
			  : tasks	? "parent_session_id"
			  : events	? "task_id"
					: "session_id";

	if (!import_has_column(scope, "incoming", table, key))
		return 0;

	int rc = import_model_read(scope, "incoming", table, &source);
	if (rc != 0)
		return rc;

	nb_vec_init(&names, sizeof(size_t));
	for (size_t i = 0; i < source.model.field_count; i++) {
		if (!revision_ignores(source.fields[i].column)) {
			size_t *index = nb_vec_push(&names);
			if (!index)
				return NB_ERR_NOMEM;

			*index = i;
		}
	}

	blake3_hasher_update(hasher, table, strlen(table) + 1);
	if (names.len == 0)
		return 0;

	rc = revision_projection_init(&projection, names.len);
	if (rc == 0)
		rc = import_projection_init(&query_fields, names.len * 2);

	if (rc == 0 && (attachments || events))
		rc = import_model_read(scope, "incoming",
				       attachments ? "messages" : "sub_agent_tasks", &auxiliary);

	if (rc != 0)
		return rc;

	const struct norm_table_ref rows = norm_table(&source.model, NB_STR_LIT("r"));
	const struct norm_table_ref related = norm_table(&auxiliary.model, NB_STR_LIT("a"));
	for (size_t i = 0; i < names.len; i++) {
		size_t index = *(size_t *)nb_vec_at(&names, i);
		query_fields.terms[i] = norm_term_column(norm_table_col(&rows, index));
		query_fields.columns[2 * i] = (struct norm_query_field){
			.value = norm_term_call(NORM_FUNCTION_TYPEOF, &query_fields.terms[i], 1),
			.into = 2 * i,
		};
		query_fields.columns[2 * i + 1] = (struct norm_query_field){
			.value = norm_term_cast(&query_fields.terms[i], NORM_TYPE_BLOB),
			.into = 2 * i + 1,
		};
	}

	const char *order = agents  ? (strcmp(table, "agent_threads") == 0 ? "agent_id" : "id")
			    : tasks ? "task_id"
			    : strcmp(table, "session_skill_activations") == 0 ? "skill_name"
			    : strcmp(table, "sync_ui_messages") == 0	      ? "seq"
			    : import_field(&source.model, "id") != SIZE_MAX   ? "id"
									      : "session_id";
	const struct norm_query_order ordering = {
		.value =
			norm_term_column(norm_table_col(&rows, import_field(&source.model, order))),
		.direction = NORM_ASC};
	struct norm_join join = {0};
	const struct norm_term selected_session =
		norm_term_column(norm_table_col(&rows, import_field(&source.model, key)));
	const struct norm_term requested_session = norm_term_value(norm_i64(session_id));
	struct norm_expr where =
		norm_compare_terms(&selected_session, NORM_COMPARE_EQ, &requested_session);

	if (attachments || events) {
		join = (struct norm_join){
			.table = &related,
			.kind = NORM_JOIN_INNER,
			.on = norm_columns_eq(
				norm_table_col(&rows, import_field(&source.model, key)),
				norm_table_col(&related,
					       import_field(&auxiliary.model,
							    attachments ? "id" : "task_id")))};
		where = norm_eq(
			norm_table_col(&related, import_field(&auxiliary.model,
							      attachments ? "session_id"
									  : "parent_session_id")),
			norm_i64(session_id));
	}

	const struct norm_result_options limits = {.max_rows = SIZE_MAX, .max_row_bytes = SIZE_MAX};
	const struct norm_query query = {
		.from = &rows,
		.joins = (attachments || events) ? &join : NULL,
		.join_count = (attachments || events) ? 1 : 0,
		.projection = &projection.model,
		.fields = query_fields.columns,
		.field_count = names.len * 2,
		.where = where,
		.order = &ordering,
		.order_count = 1,
		.result_options = &limits,
	};

	rc = orm_status(scope->database,
			norm_query_cursor_open(scope->connection, &query, NULL, &cursor, &error),
			&error);
	bool has_row = false;

	while (rc == 0) {
		rc = orm_status(scope->database, norm_cursor_next(cursor, &has_row, &error),
				&error);
		if (rc != 0 || !has_row)
			break;

		const struct revision_cell *cells = norm_cursor_row(cursor);
		for (size_t i = 0; i < names.len; i++) {
			nb_str name = source.fields[*(size_t *)nb_vec_at(&names, i)].column;
			int kind = nb_str_eq_cstr(cells[i].kind, "integer") ? 1
				   : nb_str_eq_cstr(cells[i].kind, "real")  ? 2
				   : nb_str_eq_cstr(cells[i].kind, "text")  ? 3
				   : nb_str_eq_cstr(cells[i].kind, "blob")  ? 4
				   : nb_str_eq_cstr(cells[i].kind, "null")  ? 5
									    : 0;

			if (!kind || cells[i].value.len > INT_MAX)
				return !kind ? NB_ERR_FORMAT : NB_ERR_TOO_BIG;

			int length = (int)cells[i].value.len;
			const char terminator = '\0';

			blake3_hasher_update(hasher, name.ptr, name.len);
			blake3_hasher_update(hasher, &terminator, sizeof(terminator));
			blake3_hasher_update(hasher, &kind, sizeof(kind));
			blake3_hasher_update(hasher, &length, sizeof(length));
			if (length > 0)
				blake3_hasher_update(hasher, cells[i].value.data, (size_t)length);
		}
	}

	if (rc == 0)
		rc = orm_status(scope->database, norm_cursor_close(&cursor, &error), &error);

	return rc;
}

static int import_revision(struct orm_scope *scope, int64_t session_id, char hash[65])
{
	blake3_hasher hasher;
	unsigned char digest[BLAKE3_OUT_LEN];
	int rc = 0;

	blake3_hasher_init(&hasher);
	for (size_t i = 0; rc == 0 && i < sizeof(history_tables) / sizeof(history_tables[0]); i++)
		rc = revision_table(scope, history_tables[i], session_id, &hasher);

	if (rc != 0)
		return rc;

	blake3_hasher_finalize(&hasher, digest, sizeof(digest));
	return nb_hex_encode_to(digest, sizeof(digest), false, hash, 65, NULL);
}

struct incoming_session {
	int64_t id;
	nb_str display_id;
	bool missing_display_id;
	nb_str name;
	bool missing_name;
	nb_str model;
	bool missing_model;
};
static const struct norm_field incoming_session_fields[] = {
	NORM_FIELD(struct incoming_session, id, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct incoming_session, display_id, missing_display_id, NORM_TYPE_TEXT,
			    0),
	NORM_NULLABLE_FIELD(struct incoming_session, name, missing_name, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct incoming_session, model, missing_model, NORM_TYPE_TEXT, 0),
};
static const struct norm_model incoming_session_model = {
	.schema = NB_STR_LIT("incoming"),
	.table = NB_STR_LIT("sessions"),
	.size = sizeof(struct incoming_session),
	.alignment = _Alignof(struct incoming_session),
	.fields = incoming_session_fields,
	.field_count = sizeof(incoming_session_fields) / sizeof(incoming_session_fields[0]),
};

struct import_version_row {
	int64_t version;
};
static const struct norm_field import_version_fields[] = {
	NORM_FIELD(struct import_version_row, version, NORM_TYPE_I64, 0),
};
static const struct norm_model import_version_model =
	NORM_MODEL(struct import_version_row, "schema_migrations", import_version_fields);

static int import_legacy_error_schema(struct orm_scope *scope, bool *legacy)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	struct norm_model incoming = import_version_model;
	incoming.schema = NB_STR_LIT("incoming");
	const struct norm_select_query query = {
		.where = norm_eq(norm_col(&incoming, 0), norm_i64(3)),
		.has_limit = true,
		.limit = 1,
	};

	*legacy = true;
	if (!import_has_column(scope, "incoming", "schema_migrations", "version"))
		return 0;

	int rc = orm_status(scope->database,
			    norm_select(scope->connection, &incoming, &query, NULL, &rows, &error),
			    &error);

	if (rc == 0)
		*legacy = norm_rows_count(rows) == 0;

	return rc;
}

static int import_session(struct db *database, struct orm_scope *scope,
			  const struct incoming_session *source, const char *origin,
			  struct runtime_session_import_result *result)
{
	char revision[65];
	nb_buf identity NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf title NB_CLEANUP(nb_buf_destroy) = {0};
	struct norm_rows *receipts NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct session session = {0};
	int rc = import_revision(scope, source->id, revision);

	if (rc != 0)
		return rc;

	if (!nb_buf_printf(&identity, "%s:%s:%lld", origin,
			   source->missing_display_id ? "" : source->display_id.ptr,
			   (long long)source->id))
		return NB_ERR_NOMEM;

	struct norm_error error = {0};
	if (import_has_column(scope, "incoming", "sync_session_origins", "origin")) {
		struct norm_model origins = import_schema_sync_session_origins_model;
		origins.schema = NB_STR_LIT("incoming");
		struct norm_rows *origin_rows NB_CLEANUP(orm_rows_cleanup) = NULL;
		const size_t fields[] = {1};
		const struct norm_select_query query = {
			.where = norm_eq(norm_col(&origins, 0), norm_i64(source->id)),
			.fields = fields,
			.field_count = 1,
		};

		rc = orm_status(scope->database,
				norm_select(scope->connection, &origins, &query, NULL, &origin_rows,
					    &error),
				&error);
		if (rc != 0)
			return rc;

		const struct import_schema_sync_session_origins *saved =
			norm_rows_at(origin_rows, 0);
		if (saved) {
			nb_buf_clear(&identity);
			if (!nb_buf_append(&identity, saved->origin.ptr, saved->origin.len))
				return NB_ERR_NOMEM;
		}
	}

	const struct norm_expr receipt_predicates[] = {
		norm_eq(norm_col(&import_schema_sync_import_receipts_model, 0),
			norm_text(nb_buf_str(&identity))),
		norm_eq(norm_col(&import_schema_sync_import_receipts_model, 1),
			orm_string(revision)),
	};
	const struct norm_select_query receipt_query = {.where = norm_and(receipt_predicates, 2)};

	rc = orm_status(scope->database,
			norm_select(scope->connection, &import_schema_sync_import_receipts_model,
				    &receipt_query, NULL, &receipts, &error),
			&error);
	if (rc != 0)
		return rc;

	const struct import_schema_sync_import_receipts *receipt = norm_rows_at(receipts, 0);
	if (receipt) {
		const struct norm_table_ref local = norm_table(&session_model, NB_STR_LIT("s"));
		const struct norm_query_field fields[] = {
			{.value = norm_term_value(norm_i64(source->id)), .into = 0},
			{.value = norm_term_column(norm_table_col(&local, SESSION_ID)), .into = 1},
			{.value = norm_term_value(norm_i64(0)), .into = 2},
		};
		const struct norm_query query = {
			.from = &local,
			.projection = &import_schema_import_session_map_model,
			.fields = fields,
			.field_count = 3,
			.where = norm_eq(
				norm_table_col(&local, SESSION_ID),
				norm_i64(receipt->missing_session_id ? 0 : receipt->session_id)),
		};

		rc = orm_status(scope->database,
				norm_insert_select(scope->connection,
						   &import_schema_import_session_map_model, NULL, 0,
						   &query, NULL, NULL, NULL, &error),
				&error);
		if (rc == 0)
			result->unchanged++;

		return rc;
	}

	char *base NB_CLEANUP(nb_charp_cleanup) = nb_utf8_ellipsize_bytes(
		source->missing_name ? "Imported conversation" : source->name.ptr, 180);

	if (!base || !nb_buf_append_cstr(&title, base))
		return NB_ERR_NOMEM;

	for (int suffix = 1; session_get_by_name(database, nb_buf_cstr(&title), &session) == 0;
	     suffix++) {
		nb_buf_clear(&title);
		if (!nb_buf_printf(&title, "%s [sync %d]", base, suffix))
			return NB_ERR_NOMEM;
	}

	rc = session_create(database, nb_buf_cstr(&title),
			    source->missing_model ? NULL : source->model.ptr, &session);
	if (rc != 0)
		return rc;

	const struct import_schema_import_session_map mapping = {
		.old_id = source->id,
		.new_id = session.id,
		.is_new = 1,
	};
	const struct import_schema_sync_import_receipts inserted = {
		.origin = nb_buf_str(&identity),
		.revision = nb_str_from_cstr(revision),
		.session_id = session.id,
	};
	const struct import_schema_sync_session_origins origin_row = {
		.session_id = session.id,
		.origin = nb_buf_str(&identity),
	};

	rc = orm_status(scope->database,
			norm_insert(scope->connection, &import_schema_import_session_map_model,
				    &mapping, NULL, NULL, NULL, &error),
			&error);
	if (rc == 0)
		rc = orm_status(scope->database,
				norm_insert(scope->connection,
					    &import_schema_sync_import_receipts_model, &inserted,
					    NULL, NULL, NULL, &error),
				&error);

	if (rc == 0)
		rc = orm_status(scope->database,
				norm_insert(scope->connection,
					    &import_schema_sync_session_origins_model, &origin_row,
					    NULL, NULL, NULL, &error),
				&error);

	if (rc == 0)
		result->imported++;

	return rc;
}

static int64_t import_offset(struct orm_scope *scope, const char *table)
{
	struct import_model model NB_CLEANUP(import_model_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};

	if (!import_has_column(scope, "main", table, "id"))
		return 0;

	int rc = import_model_read(scope, "main", table, &model);
	if (rc != 0)
		return rc;

	const struct norm_table_ref source = norm_table(&model.model, NB_STR_LIT("s"));
	const struct norm_term id =
		norm_term_column(norm_table_col(&source, import_field(&model.model, "id")));
	const struct norm_term fallback[] = {norm_term_call(NORM_FUNCTION_MAX, &id, 1),
					     norm_term_value(norm_i64(0))};
	const struct norm_query_field fields[] = {
		{.value = norm_term_call(NORM_FUNCTION_COALESCE, fallback, 2), .into = 0}};
	const struct norm_query query = {.from = &source,
					 .projection = &orm_integer_model,
					 .fields = fields,
					 .field_count = 1};

	rc = orm_status(scope->database,
			norm_query_select(scope->connection, &query, NULL, &rows, &error), &error);
	const struct orm_integer *row = rc == 0 ? norm_rows_at(rows, 0) : NULL;

	return rc != 0 ? rc : row ? row->value : MORPH_ERR_DB;
}

static int import_table(struct orm_scope *scope, const char *table, int64_t message_offset,
			int64_t model_offset, bool legacy_errors)
{
	struct import_model target NB_CLEANUP(import_model_cleanup) = {0};
	struct import_model source NB_CLEANUP(import_model_cleanup) = {0};
	struct import_projection projection NB_CLEANUP(import_projection_cleanup) = {0};
	struct error_migration_expression migration = {0};
	struct norm_error error = {0};
	bool migrate_error = legacy_errors && strcmp(table, "history_compaction_attempts") == 0;

	if (strcmp(table, "agent_threads") == 0 || strcmp(table, "agent_mailbox") == 0 ||
	    strcmp(table, "agent_events") == 0)
		return 0;

	if (!import_has_column(scope, "incoming", table, "session_id"))
		return 0;

	int64_t offset = import_offset(scope, table);
	if (offset < 0)
		return (int)offset;

	int rc = import_model_read(scope, "main", table, &target);
	if (rc == 0)
		rc = import_model_read(scope, "incoming", table, &source);

	if (rc == 0)
		rc = import_projection_init(&projection, target.model.field_count);

	if (rc != 0)
		return rc;

	const struct norm_table_ref rows = norm_table(&source.model, NB_STR_LIT("s"));
	const struct norm_table_ref mapping =
		norm_table(&import_schema_import_session_map_model, NB_STR_LIT("m"));
	const struct norm_term source_session =
		norm_term_column(norm_table_col(&rows, import_field(&source.model, "session_id")));
	const struct norm_term mapped_session = norm_term_column(norm_table_col(&mapping, 0));
	const struct norm_join join = {
		.table = &mapping,
		.kind = NORM_JOIN_INNER,
		.on = norm_compare_terms(&source_session, NORM_COMPARE_EQ, &mapped_session),
	};
	size_t count = 0;

	for (size_t i = 0; i < target.model.field_count; i++) {
		nb_str name = target.fields[i].column;
		size_t index = import_field(&source.model, name.ptr);
		bool audit = migrate_error && nb_str_eq_cstr(name, "legacy_error_code");

		if (index == SIZE_MAX && !audit)
			continue;

		struct norm_term *terms = &projection.terms[count * 4];
		terms[0] = norm_term_column(norm_table_col(
			&rows, audit ? import_field(&source.model, "error_code") : index));
		struct norm_term value = terms[0];

		if (nb_str_eq_cstr(name, "session_id"))
			value = norm_term_column(norm_table_col(&mapping, 1));
		else if (nb_str_eq_cstr(name, "id") || nb_str_eq_cstr(name, "superseded_by") ||
			 nb_str_eq_cstr(name, "core_message_id") ||
			 nb_str_eq_cstr(name, "summary_item_id")) {
			int64_t add = nb_str_eq_cstr(name, "core_message_id")	? message_offset
				      : nb_str_eq_cstr(name, "summary_item_id") ? model_offset
										: offset;
			terms[1] = norm_term_value(norm_i64(add));
			value = norm_term_binary(NORM_BINARY_ADD, &terms[0], &terms[1]);
		} else if (migrate_error && nb_str_eq_cstr(name, "error_code")) {
			db_error_migration_expression(&migration, terms[0]);
			value = migration.value;
		}

		import_projection_field(&projection, count++, &target.fields[i], i, value);
	}

	if (!count)
		return MORPH_ERR_DB;

	projection.model.field_count = count;
	projection.model.size = count * sizeof(struct import_value_cell);
	const struct norm_query query = {
		.from = &rows,
		.joins = &join,
		.join_count = 1,
		.projection = &projection.model,
		.fields = projection.columns,
		.field_count = count,
		.where = norm_eq(norm_table_col(&mapping, 2), norm_i64(1)),
	};

	return orm_status(scope->database,
			  norm_insert_select(scope->connection, &target.model, projection.targets,
					     count, &query, NULL, NULL, NULL, &error),
			  &error);
}

static int import_named_projection(struct import_projection *projection,
				   const struct norm_model *target,
				   const struct norm_table_ref *source, const char *const *names,
				   size_t count)
{
	int rc = import_projection_init(projection, count);

	for (size_t i = 0; rc == 0 && i < count; i++) {
		size_t target_index = import_field(target, names[i]);
		size_t source_index = import_field(source->model, names[i]);

		if (target_index == SIZE_MAX)
			return NB_ERR_FORMAT;

		if (source_index == SIZE_MAX && strcmp(names[i], "legacy_error_code") != 0)
			return NB_ERR_FORMAT;

		struct norm_term value =
			source_index == SIZE_MAX
				? norm_term_value(norm_null())
				: norm_term_column(norm_table_col(source, source_index));
		import_projection_field(projection, i, &target->fields[target_index], target_index,
					value);
	}

	return rc;
}

struct import_task_mapping {
	nb_str old_id;
	bool old_id_missing;
	nb_str new_id;
	bool new_id_missing;
};
static const struct norm_field import_task_mapping_fields[] = {
	NORM_NULLABLE_FIELD(struct import_task_mapping, old_id, old_id_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct import_task_mapping, new_id, new_id_missing, NORM_TYPE_TEXT, 0),
};
static const struct norm_model import_task_mapping_model = {
	.schema = NB_STR_LIT("temp"),
	.table = NB_STR_LIT("import_task_map"),
	.size = sizeof(struct import_task_mapping),
	.alignment = _Alignof(struct import_task_mapping),
	.fields = import_task_mapping_fields,
	.field_count = 2,
};

static int import_task_mapping_create(struct orm_scope *scope, const struct import_model *incoming,
				      const struct import_model *existing)
{
	const struct norm_table_ref tasks = norm_table(&incoming->model, NB_STR_LIT("t"));
	const struct norm_table_ref parents =
		norm_table(&import_schema_import_session_map_model, NB_STR_LIT("p"));
	const struct norm_table_ref local = norm_table(&existing->model, NB_STR_LIT("x"));
	const struct norm_join join = {
		.table = &parents,
		.kind = NORM_JOIN_INNER,
		.on = norm_columns_eq(
			norm_table_col(&tasks, import_field(&incoming->model, "parent_session_id")),
			norm_table_col(&parents, 0))};
	const struct norm_query_field collision_fields[] = {
		{.value = norm_term_value(norm_i64(1)), .into = 0}};
	const struct norm_query collision = {
		.from = &local,
		.projection = &orm_integer_model,
		.fields = collision_fields,
		.field_count = 1,
		.where = norm_columns_eq(
			norm_table_col(&local, import_field(&existing->model, "task_id")),
			norm_table_col(&tasks, import_field(&incoming->model, "task_id"))),
	};
	const struct norm_term task_id =
		norm_term_column(norm_table_col(&tasks, import_field(&incoming->model, "task_id")));
	const struct norm_term separator = norm_term_value(orm_string("@"));
	const struct norm_term parent_id = norm_term_column(norm_table_col(&parents, 1));
	const struct norm_term prefix = norm_term_binary(NORM_BINARY_CONCAT, &task_id, &separator);
	const struct norm_term suffixed = norm_term_binary(NORM_BINARY_CONCAT, &prefix, &parent_id);
	const struct norm_case rename[] = {{.when = norm_exists(&collision), .then = &suffixed}};
	const struct norm_query_field fields[] = {
		{.value = task_id, .into = 0},
		{.value = norm_term_case(rename, 1, &task_id), .into = 1},
	};
	const struct norm_query query = {
		.from = &tasks,
		.joins = &join,
		.join_count = 1,
		.projection = &import_task_mapping_model,
		.fields = fields,
		.field_count = 2,
		.where = norm_eq(norm_table_col(&parents, 2), norm_i64(1)),
	};
	struct norm_error error = {0};
	int rc = orm_status(scope->database,
			    norm_schema_create(scope->connection, &import_task_mapping_model, NULL,
					       NULL, &error),
			    &error);

	if (rc == 0)
		rc = orm_status(scope->database,
				norm_insert_select(scope->connection, &import_task_mapping_model,
						   NULL, 0, &query, NULL, NULL, NULL, &error),
				&error);

	return rc;
}

static int import_subagent_tasks(struct orm_scope *scope, bool legacy_errors)
{
	static const char *const names[] = {"task_id",		"parent_session_id",
					    "child_session_id", "agent_name",
					    "description",	"mode",
					    "status",		"result",
					    "error_code",	"legacy_error_code",
					    "iterations",	"started_at",
					    "ended_at"};
	struct import_model incoming NB_CLEANUP(import_model_cleanup) = {0};
	struct import_model target NB_CLEANUP(import_model_cleanup) = {0};
	struct import_model incoming_events NB_CLEANUP(import_model_cleanup) = {0};
	struct import_model target_events NB_CLEANUP(import_model_cleanup) = {0};
	struct import_projection projection NB_CLEANUP(import_projection_cleanup) = {0};
	struct import_projection event_projection NB_CLEANUP(import_projection_cleanup) = {0};
	struct error_migration_expression migration = {0};
	struct norm_error error = {0};
	int rc = import_model_read(scope, "incoming", "sub_agent_tasks", &incoming);

	if (rc == 0)
		rc = import_model_read(scope, "main", "sub_agent_tasks", &target);

	if (rc == 0)
		rc = import_task_mapping_create(scope, &incoming, &target);

	if (rc != 0)
		return rc;

	const struct norm_table_ref rows = norm_table(&incoming.model, NB_STR_LIT("t"));
	const struct norm_table_ref mapping =
		norm_table(&import_task_mapping_model, NB_STR_LIT("m"));
	const struct norm_table_ref parents =
		norm_table(&import_schema_import_session_map_model, NB_STR_LIT("p"));
	const struct norm_table_ref children =
		norm_table(&import_schema_import_session_map_model, NB_STR_LIT("c"));
	const struct norm_join joins[] = {
		{.table = &mapping,
		 .kind = NORM_JOIN_INNER,
		 .on = norm_columns_eq(
			 norm_table_col(&rows, import_field(&incoming.model, "task_id")),
			 norm_table_col(&mapping, 0))},
		{.table = &parents,
		 .kind = NORM_JOIN_INNER,
		 .on = norm_columns_eq(
			 norm_table_col(&rows, import_field(&incoming.model, "parent_session_id")),
			 norm_table_col(&parents, 0))},
		{.table = &children,
		 .kind = NORM_JOIN_LEFT,
		 .on = norm_columns_eq(
			 norm_table_col(&rows, import_field(&incoming.model, "child_session_id")),
			 norm_table_col(&children, 0))},
	};

	rc = import_named_projection(&projection, &target.model, &rows, names,
				     NB_ARRAY_SIZE(names));
	if (rc != 0)
		return rc;

	projection.columns[0].value = norm_term_column(norm_table_col(&mapping, 1));
	projection.columns[1].value = norm_term_column(norm_table_col(&parents, 1));
	projection.columns[2].value = norm_term_column(norm_table_col(&children, 1));
	if (legacy_errors) {
		db_error_migration_expression(&migration, projection.columns[8].value);
		projection.columns[9].value = projection.columns[8].value;
		projection.columns[8].value = migration.value;
	}

	const struct norm_query query = {.from = &rows,
					 .joins = joins,
					 .join_count = 3,
					 .projection = &projection.model,
					 .fields = projection.columns,
					 .field_count = NB_ARRAY_SIZE(names)};
	rc = orm_status(scope->database,
			norm_insert_select(scope->connection, &target.model, projection.targets,
					   NB_ARRAY_SIZE(names), &query, NULL, NULL, NULL, &error),
			&error);

	if (rc == 0)
		rc = import_model_read(scope, "incoming", "sub_agent_events", &incoming_events);

	if (rc == 0)
		rc = import_model_read(scope, "main", "sub_agent_events", &target_events);

	if (rc != 0)
		return rc;

	static const char *const event_names[] = {"task_id", "event_json", "created_at"};
	const struct norm_table_ref events = norm_table(&incoming_events.model, NB_STR_LIT("e"));
	const struct norm_join event_join = {
		.table = &mapping,
		.kind = NORM_JOIN_INNER,
		.on = norm_columns_eq(
			norm_table_col(&events, import_field(&incoming_events.model, "task_id")),
			norm_table_col(&mapping, 0))};
	rc = import_named_projection(&event_projection, &target_events.model, &events, event_names,
				     NB_ARRAY_SIZE(event_names));
	if (rc != 0)
		return rc;

	event_projection.columns[0].value = norm_term_column(norm_table_col(&mapping, 1));
	const struct norm_query event_query = {.from = &events,
					       .joins = &event_join,
					       .join_count = 1,
					       .projection = &event_projection.model,
					       .fields = event_projection.columns,
					       .field_count = NB_ARRAY_SIZE(event_names)};

	return orm_status(scope->database,
			  norm_insert_select(scope->connection, &target_events.model,
					     event_projection.targets, NB_ARRAY_SIZE(event_names),
					     &event_query, NULL, NULL, NULL, &error),
			  &error);
}

static int import_agent_threads(struct orm_scope *scope, bool legacy_errors)
{
	static const char *const names[] = {
		"root_session_id", "agent_id",	 "parent_agent_id", "session_id",
		"model",	   "effort",	 "depth",	    "description",
		"status",	   "result",	 "error_code",	    "legacy_error_code",
		"iterations",	   "started_at", "ended_at",	    "assignment_json"};
	struct import_model incoming NB_CLEANUP(import_model_cleanup) = {0};
	struct import_model target NB_CLEANUP(import_model_cleanup) = {0};
	struct import_projection projection NB_CLEANUP(import_projection_cleanup) = {0};
	struct error_migration_expression migration = {0};
	struct norm_error error = {0};
	int rc = import_model_read(scope, "incoming", "agent_threads", &incoming);

	if (rc == 0)
		rc = import_model_read(scope, "main", "agent_threads", &target);

	if (rc != 0)
		return rc;

	const struct norm_table_ref rows = norm_table(&incoming.model, NB_STR_LIT("a"));
	const struct norm_table_ref parents =
		norm_table(&import_schema_import_session_map_model, NB_STR_LIT("p"));
	const struct norm_table_ref children =
		norm_table(&import_schema_import_session_map_model, NB_STR_LIT("c"));
	const struct norm_join joins[] = {
		{.table = &parents,
		 .kind = NORM_JOIN_INNER,
		 .on = norm_columns_eq(
			 norm_table_col(&rows, import_field(&incoming.model, "root_session_id")),
			 norm_table_col(&parents, 0))},
		{.table = &children,
		 .kind = NORM_JOIN_INNER,
		 .on = norm_columns_eq(
			 norm_table_col(&rows, import_field(&incoming.model, "session_id")),
			 norm_table_col(&children, 0))},
	};
	size_t count = import_field(&incoming.model, "assignment_json") == SIZE_MAX
			       ? NB_ARRAY_SIZE(names) - 1
			       : NB_ARRAY_SIZE(names);
	rc = import_named_projection(&projection, &target.model, &rows, names, count);
	if (rc != 0)
		return rc;

	projection.columns[0].value = norm_term_column(norm_table_col(&parents, 1));
	projection.columns[3].value = norm_term_column(norm_table_col(&children, 1));
	const struct norm_term status = projection.columns[8].value;
	const struct norm_term cancelled = norm_term_value(norm_i64(4));
	const struct norm_value active[] = {norm_i64(0), norm_i64(1)};
	const struct norm_case cancel[] = {
		{.when = norm_in(norm_table_col(&rows, import_field(&incoming.model, "status")),
				 active, 2),
		 .then = &cancelled}};
	projection.columns[8].value = norm_term_case(cancel, 1, &status);
	if (legacy_errors) {
		db_error_migration_expression(&migration, projection.columns[10].value);
		projection.columns[11].value = projection.columns[10].value;
		projection.columns[10].value = migration.value;
	}

	const struct norm_query query = {.from = &rows,
					 .joins = joins,
					 .join_count = 2,
					 .projection = &projection.model,
					 .fields = projection.columns,
					 .field_count = count,
					 .where =
						 norm_eq(norm_table_col(&parents, 2), norm_i64(1))};

	return orm_status(scope->database,
			  norm_insert_select(scope->connection, &target.model, projection.targets,
					     count, &query, NULL, NULL, NULL, &error),
			  &error);
}

static int import_agent_rows(struct orm_scope *scope, const char *table, const char *const *names,
			     size_t count, bool budget)
{
	struct import_model incoming NB_CLEANUP(import_model_cleanup) = {0};
	struct import_model target NB_CLEANUP(import_model_cleanup) = {0};
	struct import_projection projection NB_CLEANUP(import_projection_cleanup) = {0};
	struct norm_error error = {0};
	int rc = import_model_read(scope, "incoming", table, &incoming);

	if (rc == 0)
		rc = import_model_read(scope, "main", table, &target);

	if (rc != 0)
		return rc;

	const struct norm_table_ref rows = norm_table(&incoming.model, NB_STR_LIT("a"));
	const struct norm_table_ref parents =
		norm_table(&import_schema_import_session_map_model, NB_STR_LIT("p"));
	const struct norm_join join = {
		.table = &parents,
		.kind = NORM_JOIN_INNER,
		.on = norm_columns_eq(
			norm_table_col(&rows, import_field(&incoming.model, "root_session_id")),
			norm_table_col(&parents, 0))};
	rc = import_named_projection(&projection, &target.model, &rows, names, count);
	if (rc != 0)
		return rc;

	projection.columns[0].value = norm_term_column(norm_table_col(&parents, 1));
	const struct norm_query query = {
		.from = &rows,
		.joins = &join,
		.join_count = 1,
		.projection = &projection.model,
		.fields = projection.columns,
		.field_count = count,
		.where = budget ? (struct norm_expr){0}
				: norm_eq(norm_table_col(&parents, 2), norm_i64(1))};
	struct norm_term arguments[5][2] = {0};
	struct norm_term greatest[5] = {0};
	struct norm_assignment set[5] = {0};
	const size_t keys[] = {import_field(&target.model, "root_session_id")};

	if (budget) {
		for (size_t i = 0; i < 5; i++) {
			size_t field = import_field(&target.model, names[i + 1]);
			arguments[i][0] = norm_term_column(norm_col(&target.model, field));
			arguments[i][1] = norm_term_excluded(norm_col(&target.model, field));
			greatest[i] = norm_term_call(NORM_FUNCTION_GREATEST, arguments[i], 2);
			set[i] = (struct norm_assignment){.column = norm_col(&target.model, field),
							  .expression = &greatest[i]};
		}
	}

	const struct norm_conflict conflict = {.action = NORM_CONFLICT_UPDATE,
					       .fields = keys,
					       .field_count = 1,
					       .set = set,
					       .set_count = 5};

	return orm_status(scope->database,
			  norm_insert_select(scope->connection, &target.model, projection.targets,
					     count, &query, budget ? &conflict : NULL, NULL, NULL,
					     &error),
			  &error);
}

static int import_attachments(struct orm_scope *scope, int64_t message_offset)
{
	static const char *const names[] = {"message_id", "kind", "path", "sha256"};
	struct import_model incoming NB_CLEANUP(import_model_cleanup) = {0};
	struct import_model target NB_CLEANUP(import_model_cleanup) = {0};
	struct import_projection projection NB_CLEANUP(import_projection_cleanup) = {0};
	struct norm_model incoming_messages = message_model;
	incoming_messages.schema = NB_STR_LIT("incoming");
	struct norm_error error = {0};
	int rc = import_model_read(scope, "incoming", "message_attachments", &incoming);

	if (rc == 0)
		rc = import_model_read(scope, "main", "message_attachments", &target);

	if (rc != 0)
		return rc;

	const struct norm_table_ref rows = norm_table(&incoming.model, NB_STR_LIT("a"));
	const struct norm_table_ref messages = norm_table(&incoming_messages, NB_STR_LIT("s"));
	const struct norm_table_ref parents =
		norm_table(&import_schema_import_session_map_model, NB_STR_LIT("m"));
	const struct norm_join joins[] = {
		{.table = &messages,
		 .kind = NORM_JOIN_INNER,
		 .on = norm_columns_eq(
			 norm_table_col(&rows, import_field(&incoming.model, "message_id")),
			 norm_table_col(&messages, MESSAGE_ID))},
		{.table = &parents,
		 .kind = NORM_JOIN_INNER,
		 .on = norm_columns_eq(norm_table_col(&messages, MESSAGE_SESSION_ID),
				       norm_table_col(&parents, 0))},
	};
	rc = import_named_projection(&projection, &target.model, &rows, names,
				     NB_ARRAY_SIZE(names));
	if (rc != 0)
		return rc;

	const struct norm_term old_id = projection.columns[0].value;
	const struct norm_term offset = norm_term_value(norm_i64(message_offset));
	projection.columns[0].value = norm_term_binary(NORM_BINARY_ADD, &old_id, &offset);
	const struct norm_query query = {.from = &rows,
					 .joins = joins,
					 .join_count = 2,
					 .projection = &projection.model,
					 .fields = projection.columns,
					 .field_count = NB_ARRAY_SIZE(names),
					 .where =
						 norm_eq(norm_table_col(&parents, 2), norm_i64(1))};

	return orm_status(scope->database,
			  norm_insert_select(scope->connection, &target.model, projection.targets,
					     NB_ARRAY_SIZE(names), &query, NULL, NULL, NULL,
					     &error),
			  &error);
}

struct import_optional_integer {
	int64_t value;
	bool missing;
};
static const struct norm_field import_optional_integer_fields[] = {
	NORM_NULLABLE_FIELD(struct import_optional_integer, value, missing, NORM_TYPE_I64, 0),
};
static const struct norm_model import_optional_integer_model = {
	.size = sizeof(struct import_optional_integer),
	.alignment = _Alignof(struct import_optional_integer),
	.fields = import_optional_integer_fields,
	.field_count = 1,
};

static int import_foreign_keys(struct orm_scope *scope, const char *schema)
{
	struct norm_error error = {0};
	size_t issues = 0;
	int rc = orm_status(scope->database,
			    norm_sqlite_check_schema(scope->connection, nb_str_from_cstr(schema),
						     NORM_SQLITE_CHECK_FOREIGN_KEYS, NULL, NULL,
						     NULL, &issues, &error),
			    &error);

	return rc == 0 && issues == 0 ? 0 : NB_ERR_FORMAT;
}

static int import_validate_source(struct orm_scope *scope)
{
	struct norm_model messages = message_model;
	struct norm_model sessions = session_model;
	messages.schema = NB_STR_LIT("incoming");
	sessions.schema = NB_STR_LIT("incoming");
	const struct norm_table_ref child = norm_table(&messages, NB_STR_LIT("m"));
	const struct norm_table_ref parent = norm_table(&sessions, NB_STR_LIT("s"));
	const struct norm_join join = {
		.table = &parent,
		.kind = NORM_JOIN_LEFT,
		.on = norm_columns_eq(norm_table_col(&child, MESSAGE_SESSION_ID),
				      norm_table_col(&parent, SESSION_ID))};
	const struct norm_join_column columns[] = {
		{.column = norm_table_col(&child, MESSAGE_ID), .into = 0}};
	const struct norm_join_query orphans = {
		.from = &child,
		.joins = &join,
		.join_count = 1,
		.projection = &orm_integer_model,
		.columns = columns,
		.column_count = 1,
		.select = {.where = norm_is_null(norm_table_col(&parent, SESSION_ID)),
			   .has_limit = true,
			   .limit = 1},
	};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	int rc = import_foreign_keys(scope, "incoming");

	if (rc == 0)
		rc = orm_status(scope->database,
				norm_join_select(scope->connection, &orphans, NULL, &rows, &error),
				&error);

	if (rc != 0 || norm_rows_count(rows) != 0)
		return NB_ERR_FORMAT;

	orm_rows_cleanup(&rows);
	if (!import_has_column(scope, "incoming", "schema_migrations", "version"))
		return 0;

	struct norm_model models[2] = {import_version_model, import_version_model};
	models[0].schema = NB_STR_LIT("incoming");
	models[1].schema = NB_STR_LIT("main");
	struct norm_table_ref sources[2] = {0};
	struct norm_term versions[2] = {0};
	struct norm_term defaults[2][2] = {0};
	struct norm_query_field aggregates[2] = {0};
	struct norm_query queries[2] = {0};
	for (size_t i = 0; i < 2; i++) {
		sources[i] = norm_table(&models[i], NB_STR_LIT("v"));
		versions[i] = norm_term_column(norm_table_col(&sources[i], 0));
		defaults[i][0] = norm_term_call(NORM_FUNCTION_MAX, &versions[i], 1);
		defaults[i][1] = norm_term_value(norm_i64(0));
		aggregates[i] = (struct norm_query_field){
			.value = norm_term_call(NORM_FUNCTION_COALESCE, defaults[i], 2), .into = 0};
		queries[i] = (struct norm_query){.from = &sources[i],
						 .projection = &orm_integer_model,
						 .fields = &aggregates[i],
						 .field_count = 1};
	}

	const struct norm_term incoming = norm_term_scalar(&queries[0]);
	const struct norm_term current = norm_term_scalar(&queries[1]);
	const struct norm_query_field selected[] = {
		{.value = norm_term_value(norm_i64(1)), .into = 0}};
	const struct norm_query newer = {
		.projection = &orm_integer_model,
		.fields = selected,
		.field_count = 1,
		.where = norm_compare_terms(&incoming, NORM_COMPARE_GT, &current)};
	rc = orm_status(scope->database,
			norm_query_select(scope->connection, &newer, NULL, &rows, &error), &error);

	return rc == 0 && norm_rows_count(rows) == 0 ? 0 : NB_ERR_FORMAT;
}

static int import_restore_session_metadata(struct orm_scope *scope)
{
	struct norm_model incoming = session_model;
	incoming.schema = NB_STR_LIT("incoming");
	const struct norm_table_ref sessions = norm_table(&incoming, NB_STR_LIT("s"));
	const struct norm_table_ref mapping =
		norm_table(&import_schema_import_session_map_model, NB_STR_LIT("m"));
	const struct norm_join join = {.table = &mapping,
				       .kind = NORM_JOIN_INNER,
				       .on = norm_columns_eq(norm_table_col(&sessions, SESSION_ID),
							     norm_table_col(&mapping, 0))};
	const struct norm_expr conditions[] = {
		norm_columns_eq(norm_table_col(&mapping, 1), norm_col(&session_model, SESSION_ID)),
		norm_eq(norm_table_col(&mapping, 2), norm_i64(1)),
	};
	const size_t restored[] = {SESSION_CREATED_AT, SESSION_UPDATED_AT, SESSION_TOKEN_USED};
	struct norm_query_field fields[3] = {0};
	struct norm_query sources[3] = {0};
	struct norm_term values[3] = {0};
	struct norm_assignment set[3] = {0};

	for (size_t i = 0; i < 3; i++) {
		fields[i] = (struct norm_query_field){
			.value = norm_term_column(norm_table_col(&sessions, restored[i])),
			.into = 0};
		sources[i] = (struct norm_query){.from = &sessions,
						 .joins = &join,
						 .join_count = 1,
						 .projection = &import_optional_integer_model,
						 .fields = &fields[i],
						 .field_count = 1,
						 .where = norm_and(conditions, 2)};
		values[i] = norm_term_scalar(&sources[i]);
		set[i] = (struct norm_assignment){.column = norm_col(&session_model, restored[i]),
						  .expression = &values[i]};
	}

	const struct norm_query_field mapped_id[] = {
		{.value = norm_term_column(norm_table_col(&mapping, 1)), .into = 0}};
	const struct norm_query changed = {
		.from = &mapping,
		.projection = &import_optional_integer_model,
		.fields = mapped_id,
		.field_count = 1,
		.where = norm_eq(norm_table_col(&mapping, 2), norm_i64(1))};
	const struct norm_term target_id = norm_term_column(norm_col(&session_model, SESSION_ID));
	const struct norm_update_query update = {
		.set = set, .set_count = 3, .where = norm_in_query(&target_id, &changed, false)};
	struct norm_error error = {0};

	return orm_status(
		scope->database,
		norm_update(scope->connection, &session_model, &update, NULL, NULL, &error),
		&error);
}

static int import_file_execute(struct db *database, struct orm_scope *scope, const char *path,
			       const char *origin, struct runtime_session_import_result *result)
{
	struct norm_rows *sessions NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	bool legacy_errors = false;
	int rc = orm_status(database,
			    norm_sqlite_set_trusted_schema(scope->connection, false, NULL, &error),
			    &error);

	if (rc == 0)
		rc = orm_status(database,
				norm_sqlite_attach(scope->connection, nb_str_from_cstr(path),
						   NB_STR_LIT("incoming"), NULL, &error),
				&error);

	if (rc != 0)
		return rc;

	if (!import_has_column(scope, "incoming", "sessions", "display_id") ||
	    !import_has_column(scope, "incoming", "messages", "session_id"))
		return NB_ERR_FORMAT;

	rc = import_validate_source(scope);
	if (rc == 0)
		rc = orm_transaction_begin(scope, NORM_SQLITE_IMMEDIATE);

	if (rc == 0)
		rc = import_legacy_error_schema(scope, &legacy_errors);

	if (rc == 0)
		rc = import_schema(scope);

	const struct norm_order order = {.column = norm_col(&incoming_session_model, 0),
					 .direction = NORM_ASC};
	const struct norm_select_query incoming_query = {
		.order = &order, .order_count = 1, .max_rows = SIZE_MAX, .max_bytes = SIZE_MAX};
	if (rc == 0)
		rc = orm_status(database,
				norm_select(scope->connection, &incoming_session_model,
					    &incoming_query, NULL, &sessions, &error),
				&error);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(sessions); i++)
		rc = import_session(database, scope, norm_rows_at(sessions, i), origin, result);

	int64_t message_offset = rc == 0 ? import_offset(scope, "messages") : 0;
	int64_t model_offset = rc == 0 ? import_offset(scope, "model_history_items") : 0;
	if (message_offset < 0 || model_offset < 0)
		rc = MORPH_ERR_DB;

	if (rc == 0)
		rc = import_restore_session_metadata(scope);

	for (size_t i = 0; rc == 0 && i < NB_ARRAY_SIZE(history_tables); i++)
		rc = import_table(scope, history_tables[i], message_offset, model_offset,
				  legacy_errors);

	if (rc == 0 && import_has_column(scope, "incoming", "message_attachments", "message_id"))
		rc = import_attachments(scope, message_offset);

	if (rc == 0 && import_has_column(scope, "incoming", "sub_agent_tasks", "task_id"))
		rc = import_subagent_tasks(scope, legacy_errors);

	if (rc == 0 && import_has_column(scope, "incoming", "agent_threads", "root_session_id"))
		rc = import_agent_threads(scope, legacy_errors);

	static const char *const mailbox[] = {
		"root_session_id", "sender", "recipient", "kind", "body", "consumed"};
	static const char *const events[] = {"root_session_id", "agent_id", "event_json"};
	static const char *const budget[] = {"root_session_id", "used",		 "input_tokens",
					     "cached_tokens",	"output_tokens", "reminder_level"};
	if (rc == 0 && import_has_column(scope, "incoming", "agent_mailbox", "root_session_id"))
		rc = import_agent_rows(scope, "agent_mailbox", mailbox, NB_ARRAY_SIZE(mailbox),
				       false);

	if (rc == 0 && import_has_column(scope, "incoming", "agent_events", "root_session_id"))
		rc = import_agent_rows(scope, "agent_events", events, NB_ARRAY_SIZE(events), false);

	if (rc == 0 &&
	    import_has_column(scope, "incoming", "agent_budget_state", "root_session_id"))
		rc = import_agent_rows(scope, "agent_budget_state", budget, NB_ARRAY_SIZE(budget),
				       true);

	if (rc == 0)
		rc = import_foreign_keys(scope, "main");

	return rc;
}

int orm_session_import_file(struct db *target, const char *path, const char *origin,
			    struct runtime_session_import_result *result)
{
	struct db database NB_CLEANUP(db_close) = {0};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};

	if (!target || !db_is_open(target) || !path || !origin || !result)
		return NB_ERR_INVAL;

	memset(result, 0, sizeof(*result));
	if (!nb_file_exists(path))
		return NB_ERR_NOT_FOUND;

	int rc = db_open_plain(target->logger, &database, target->path);

	if (rc == 0)
		rc = orm_scope_begin(&database, &scope);

	if (rc == 0)
		rc = import_file_execute(&database, &scope, path, origin, result);

	if (rc == 0)
		rc = orm_transaction_commit(&scope);

	if (scope.connection && rc != 0 && norm_tx_get_state(scope.connection) != NORM_TX_IDLE)
		(void)orm_transaction_rollback(&scope);

	if (scope.connection) {
		nb_status detached =
			norm_sqlite_detach(scope.connection, NB_STR_LIT("incoming"), NULL, &error);

		if (rc == 0)
			rc = orm_status(&database, detached, &error);
	}

	if (rc != 0)
		memset(result, 0, sizeof(*result));

	return rc;
}
