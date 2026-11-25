#include "persistence/orm/error_migration.h"
#include "error_migration_models.h"
#include "schema_models.h"
#include "persistence/orm/internal.h"
#include "persistence/orm/records.h"
#include "errors.h"
#include <string.h>

/* Historical error domains are fixed. Only common unambiguous errno values
 * can be mapped precisely because legacy rows did not record their source OS. */
static const struct error_mapping {
	int legacy;
	int current;
} error_mappings[] = {
	{-257, MORPH_ERR_NOT_CONFIGURED},
	{-258, MORPH_ERR_NOT_INITIALIZED},
	{-259, MORPH_ERR_API},
	{-260, MORPH_ERR_NETWORK},
	{-261, NB_ERR_PARSE},
	{-262, NB_ERR_PROTOCOL},
	{-263, MORPH_ERR_DB},
	{-264, NB_ERR_FORMAT},
	{-265, MORPH_ERR_PROCESSING},
	{-266, MORPH_ERR_SANDBOX},
	{-267, MORPH_ERR_LOAD},
	{-268, MORPH_ERR_LLM},
	{-269, MORPH_ERR_REACT_MAX_ITERATIONS},
	{-270, MORPH_ERR_CONFIG},
	{-271, MORPH_ERR_CONTEXT_LIMIT},
	{-272, MORPH_ERR_TOKEN_BUDGET},
	{-273, MORPH_ERR_AGENT_BUDGET},
	{-274, MORPH_ERR_REQUEST_BUDGET},
	{-1, NB_ERR_NOT_PERMITTED},
	{-2, NB_ERR_NOT_FOUND},
	{-4, NB_ERR_INTERRUPTED},
	{-5, NB_ERR_IO},
	{-7, NB_ERR_TOO_BIG},
	{-9, NB_ERR_BAD_HANDLE},
	{-10, NB_ERR_NO_CHILD},
	{-12, NB_ERR_NOMEM},
	{-13, NB_ERR_PERMISSION},
	{-16, NB_ERR_BUSY},
	{-17, NB_ERR_EXISTS},
	{-18, NB_ERR_CROSS_DEVICE},
	{-20, NB_ERR_NOT_DIR},
	{-21, NB_ERR_IS_DIR},
	{-22, NB_ERR_INVAL},
	{-24, NB_ERR_RESOURCE},
	{-25, NB_ERR_NOT_TERMINAL},
	{-27, NB_ERR_TOO_BIG},
	{-28, NB_ERR_NO_SPACE},
	{-32, NB_ERR_BROKEN_PIPE},
	{-34, NB_ERR_RANGE},
};

void db_error_migration_expression(struct error_migration_expression *out, struct norm_term input)
{
	memset(out, 0, sizeof(*out));
	out->input = input;
	out->zero = norm_term_value(norm_i32(0));
	out->unknown = norm_term_value(norm_i32(NB_ERR_OS));
	for (size_t i = 0; i < NB_ARRAY_SIZE(error_mappings); i++) {
		out->legacy[i] = norm_term_value(norm_i32(error_mappings[i].legacy));
		out->current[i] = norm_term_value(norm_i32(error_mappings[i].current));
		out->cases[i].when =
			norm_compare_terms(&out->input, NORM_COMPARE_EQ, &out->legacy[i]);
		out->cases[i].then = &out->current[i];
	}

	out->positive.when = norm_compare_terms(&out->input, NORM_COMPARE_GE, &out->zero);
	out->positive.then = &out->input;
	out->fallback = norm_term_case(&out->positive, 1, &out->unknown);
	out->value = norm_term_case(out->cases, NB_ARRAY_SIZE(out->cases), &out->fallback);
}

static int error_migration_table(struct orm_scope *scope, const struct norm_model *model)
{
	bool has = false;
	size_t legacy = schema_field(model, "legacy_error_code");
	size_t current = schema_field(model, "error_code");
	struct norm_error error = {0};
	int rc = orm_table_has_column(scope, "main", model->table.ptr, "legacy_error_code", &has);

	if (rc != NB_OK)
		return rc;

	if (!has) {
		rc = orm_status(
			scope->database,
			norm_schema_add_column(scope->connection, model, legacy, NULL, &error),
			&error);
		if (rc != NB_OK)
			return rc;
	}

	const struct norm_term input = norm_term_column(norm_col(model, current));
	struct error_migration_expression mapped = {0};

	db_error_migration_expression(&mapped, input);
	const struct norm_assignment assignments[] = {
		{.column = norm_col(model, legacy), .expression = &input},
		{.column = norm_col(model, current), .expression = &mapped.value},
	};
	const struct norm_update_query query = {.set = assignments,
						.set_count = NB_ARRAY_SIZE(assignments),
						.allow_all_rows = true};

	return orm_status(scope->database,
			  norm_update(scope->connection, model, &query, NULL, NULL, &error),
			  &error);
}

int db_migrate_error_codes(struct db *db)
{
	const enum schema_table owners[] = {SCHEMA_MEMORY_JOBS, SCHEMA_SUB_AGENT_TASKS,
					    SCHEMA_AGENT_THREADS,
					    SCHEMA_HISTORY_COMPACTION_ATTEMPTS};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	bool applied = false;
	int rc = orm_scope_begin(db, &scope);

	if (rc == NB_OK)
		rc = schema_version_read(&scope, 3, &applied);

	if (rc != NB_OK || applied)
		return rc;

	rc = orm_savepoint_begin(&scope, "nbase_error_codes");
	if (rc != NB_OK)
		return rc;

	for (size_t i = 0; rc == NB_OK && i < NB_ARRAY_SIZE(owners); i++)
		rc = error_migration_table(&scope, schema_model(owners[i]));

	if (rc == NB_OK)
		rc = schema_version_record(&scope, 3, "nbase_error_codes");

	return orm_savepoint_finish(&scope, "nbase_error_codes", rc);
}
