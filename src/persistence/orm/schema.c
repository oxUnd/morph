#include "database.h"
#include "internal.h"
#include "schema_models.h"
#include "history_models.h"
#include "error_migration.h"
#include "errors.h"
#include <norm/query.h>
#include <nb/vec.h>
#include <time.h>

struct schema_version_row {
	int64_t version;
	nb_str name;
	int64_t applied_at;
};
static const struct norm_field version_fields[] = {
	NORM_FIELD(struct schema_version_row, version, NORM_TYPE_I64, NORM_FIELD_PRIMARY_KEY),
	NORM_FIELD(struct schema_version_row, name, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct schema_version_row, applied_at, NORM_TYPE_I64, 0),
};
static const struct norm_model version_model =
	NORM_MODEL(struct schema_version_row, "schema_migrations", version_fields);

int schema_version_read(struct orm_scope *scope, int64_t version, bool *applied)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	const size_t field = 0;
	const struct norm_select_query query = {
		.fields = &field,
		.field_count = 1,
		.where = norm_eq(norm_col(&version_model, 0), norm_i64(version)),
		.has_limit = true,
		.limit = 1};
	int rc = orm_select_rows(scope, &version_model, &query, &rows);

	if (rc == NB_OK)
		*applied = norm_rows_count(rows) != 0;

	return rc;
}

int schema_version_record(struct orm_scope *scope, int64_t version, const char *name)
{
	const struct schema_version_row row = {.version = version,
					       .name = nb_str_from_cstr(name),
					       .applied_at = (int64_t)time(NULL)};
	const struct norm_conflict conflict = {.action = NORM_CONFLICT_NOTHING};
	const struct norm_insert_options options = {.conflict = &conflict};

	return orm_insert_row(scope, &version_model, &row, &options, NULL);
}

static void metadata_cleanup(struct norm_table_metadata **owner)
{
	norm_table_metadata_free(*owner);
	*owner = NULL;
}

int orm_table_has_column(struct orm_scope *scope, const char *schema, const char *table,
			 const char *column, bool *has)
{
	struct norm_table_metadata *metadata NB_CLEANUP(metadata_cleanup) = NULL;
	struct norm_error error = {0};

	if (!scope || !has || !table || !column)
		return NB_ERR_INVAL;

	*has = false;
	int rc = norm_schema_read_table(scope->connection,
					nb_str_from_cstr(schema ? schema : "main"),
					nb_str_from_cstr(table), NULL, NULL, &metadata, &error);

	if (rc == NB_ERR_NOT_FOUND)
		return NB_OK;

	if (rc != NB_OK)
		return orm_status(scope->database, rc, &error);

	for (size_t i = 0; i < norm_table_metadata_column_count(metadata); i++) {
		if (nb_str_eq_cstr(norm_table_metadata_column_at(metadata, i)->name, column)) {
			*has = true;
			break;
		}
	}

	return NB_OK;
}

static int schema_upgrade_columns(struct orm_scope *scope)
{
	static const struct {
		enum schema_table table;
		const char *column;
	} additions[] = {
		{SCHEMA_SESSIONS, "workdir"},
		{SCHEMA_MEMORY_SCOPES, "content_revision"},
		{SCHEMA_MEMORY_JOBS, "content_revision"},
		{SCHEMA_MEMORY_JOBS, "stage"},
		{SCHEMA_MEMORY_PREFERENCES, "origin"},
		{SCHEMA_MEMORY_PREFERENCES, "source_order"},
		{SCHEMA_MEMORY_SCOPES, "generation"},
		{SCHEMA_MEMORY_EPISODES, "job_id"},
		{SCHEMA_MEMORY_FACTS, "category"},
		{SCHEMA_MEMORY_FACTS, "importance"},
		{SCHEMA_MEMORY_EPISODES, "key_decisions"},
		{SCHEMA_MEMORY_EPISODES, "artifacts"},
		{SCHEMA_MEMORY_EPISODES, "tools_used"},
		{SCHEMA_MEMORY_EPISODES, "importance"},
		{SCHEMA_MESSAGES, "turn_id"},
		{SCHEMA_OUTPUTS, "request_prompt"},
		{SCHEMA_OUTPUTS, "provider"},
		{SCHEMA_OUTPUTS, "tool_name"},
		{SCHEMA_OUTPUTS, "tool_call_id"},
		{SCHEMA_OUTPUTS, "turn_id"},
		{SCHEMA_OUTPUTS, "recipe_json"},
		{SCHEMA_OUTPUTS, "mime"},
		{SCHEMA_OUTPUTS, "width"},
		{SCHEMA_OUTPUTS, "height"},
		{SCHEMA_OUTPUTS, "duration_seconds"},
		{SCHEMA_OUTPUTS, "size_bytes"},
		{SCHEMA_SCHEDULED_TASKS, "run_token"},
		{SCHEMA_SCHEDULED_TASKS, "source_session_id"},
		{SCHEMA_SCHEDULED_TASKS, "latest_session_id"},
		{SCHEMA_NOTIFICATIONS, "session_id"},
		{SCHEMA_AGENT_THREADS, "assignment_json"},
		{SCHEMA_MODEL_HISTORY_ITEMS, "idempotency_key"},
		{SCHEMA_SESSIONS, "display_id"},
	};
	struct norm_error error = {0};

	for (size_t table = 0; table < SCHEMA_TABLE_COUNT; table++) {
		bool required = false;

		for (size_t i = 0; i < NB_ARRAY_SIZE(additions); i++)
			required |= additions[i].table == (enum schema_table)table;

		if (!required)
			continue;

		const struct norm_model *model = schema_model((enum schema_table)table);
		struct norm_table_metadata *metadata NB_CLEANUP(metadata_cleanup) = NULL;
		int rc = orm_status(scope->database,
				    norm_schema_read_table(scope->connection, NB_STR_LIT("main"),
							   model->table, NULL, NULL, &metadata,
							   &error),
				    &error);

		if (rc != NB_OK)
			return rc;

		for (size_t i = 0; i < NB_ARRAY_SIZE(additions); i++) {
			if (additions[i].table != (enum schema_table)table)
				continue;

			bool has = false;
			for (size_t column = 0; column < norm_table_metadata_column_count(metadata);
			     column++)
				has |= nb_str_eq_cstr(
					norm_table_metadata_column_at(metadata, column)->name,
					additions[i].column);

			if (has)
				continue;

			size_t field = schema_field(model, additions[i].column);

			if (field == SIZE_MAX)
				return NB_ERR_STATE;

			const struct norm_model *upgrade = model;
			struct norm_model additive = *model;
			nb_vec fields NB_CLEANUP(nb_vec_cleanup) = {0};

			if (additions[i].table == SCHEMA_SESSIONS &&
			    strcmp(additions[i].column, "display_id") == 0) {
				nb_vec_init(&fields, sizeof(*model->fields));
				struct norm_field *copy =
					nb_vec_push_n(&fields, model->field_count);

				if (!copy)
					return NB_ERR_NOMEM;

				memcpy(copy, model->fields, model->field_count * sizeof(*copy));
				copy[field].flags &= ~(unsigned)NORM_FIELD_UNIQUE;
				additive.fields = copy;
				upgrade = &additive;
			}

			rc = orm_status(scope->database,
					norm_schema_add_column(scope->connection, upgrade, field,
							       NULL, &error),
					&error);
			if (rc != NB_OK)
				return rc;
		}
	}

	return NB_OK;
}

static int db_retire_memory_receipts(struct orm_scope *scope)
{
	bool applied = false;
	int rc = schema_version_read(scope, 2, &applied);

	if (rc != NB_OK || applied)
		return rc;

	rc = orm_savepoint_begin(scope, "retire_memory_receipts");
	if (rc != NB_OK)
		return rc;

	const char *prefix = "[Background receipt] memory_consolidation:";
	const struct norm_term parts[] = {
		norm_term_column(norm_col(&history_model, HIST_CONTENT)),
		norm_term_value(norm_i32(1)),
		norm_term_value(norm_i64((int64_t)strlen(prefix))),
	};
	const struct norm_term substring =
		norm_term_call(NORM_FUNCTION_SUBSTR, parts, NB_ARRAY_SIZE(parts));
	const struct norm_term expected = norm_term_value(orm_string(prefix));
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&history_model, HIST_KIND), orm_string("background_receipt")),
		norm_eq(norm_col(&history_model, HIST_ACTIVE), norm_i32(1)),
		norm_compare_terms(&substring, NORM_COMPARE_EQ, &expected),
	};
	const struct norm_assignment inactive = {.column = norm_col(&history_model, HIST_ACTIVE),
						 .value = norm_i32(0)};

	rc = orm_update_rows(scope, &history_model, &inactive, 1,
			     norm_and(conditions, NB_ARRAY_SIZE(conditions)), NULL);
	if (rc == NB_OK)
		rc = schema_version_record(scope, 2, "retire_memory_maintenance_receipts");

	return orm_savepoint_finish(scope, "retire_memory_receipts", rc);
}

int db_init_credit_schema(struct db *db)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc : schema_create_credit(&scope);
}

int db_init_schema(struct db *db)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);

	if (rc == NB_OK)
		rc = schema_create_tables(&scope);

	if (rc == NB_OK)
		rc = schema_upgrade_columns(&scope);

	if (rc == NB_OK)
		rc = schema_create_indexes(&scope);

	if (rc == NB_OK)
		rc = db_retire_memory_receipts(&scope);

	if (rc == NB_OK)
		rc = schema_create_revision_triggers(&scope);

	if (rc == NB_OK)
		rc = db_migrate_error_codes(db);

	if (rc == NB_OK)
		rc = schema_version_record(&scope, 1, "baseline");

	return rc;
}
