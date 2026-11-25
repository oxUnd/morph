#include "hook_store.h"
#include "records.h"
#include <norm/query.h>
#include "errors.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct hook_trust_row {
	nb_str id;
	nb_str digest;
	int32_t disabled;
	nb_str definition;
};

static const struct norm_field hook_trust_fields[] = {
	NORM_TEXT_FIELD(struct hook_trust_row, id, 64, NORM_FIELD_PRIMARY_KEY),
	NORM_TEXT_FIELD(struct hook_trust_row, digest, 0, 0),
	{.column = NB_STR_LIT("disabled"),
	 .type = NORM_TYPE_I32,
	 .offset = offsetof(struct hook_trust_row, disabled),
	 .size = sizeof(((struct hook_trust_row *)0)->disabled),
	 .null_offset = SIZE_MAX,
	 .default_sql = NB_STR_LIT("0")},
	NORM_TEXT_FIELD(struct hook_trust_row, definition, 0, 0),
};

static const struct norm_model hook_trust_model =
	NORM_MODEL(struct hook_trust_row, "hook_trust", hook_trust_fields);

struct hook_workspace_row {
	nb_str path;
};

static const struct norm_field hook_workspace_fields[] = {
	NORM_TEXT_FIELD(struct hook_workspace_row, path, PATH_MAX - 1, NORM_FIELD_PRIMARY_KEY),
};

static const struct norm_model hook_workspace_model =
	NORM_MODEL(struct hook_workspace_row, "hook_workspaces", hook_workspace_fields);

void hook_store_trust_cleanup(struct store_hook_trust *record)
{
	free(record->digest);
	free(record->definition);
	memset(record, 0, sizeof(*record));
}

int hook_store_init(struct db *db)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_schema_options options = {.if_not_exists = true};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(db,
				norm_schema_create(scope.connection, &hook_trust_model, &options,
						   orm_scope_call(&scope), &error),
				&error);

	if (rc == 0)
		rc = orm_status(db,
				norm_schema_create(scope.connection, &hook_workspace_model,
						   &options, orm_scope_call(&scope), &error),
				&error);

	return rc;
}

int hook_store_trust_copy(struct db *db, const char *id, struct store_hook_trust *record)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	struct norm_select_query query = {
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
		.where = norm_eq(norm_col(&hook_trust_model, 0), orm_string(id))};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(db,
				norm_select(scope.connection, &hook_trust_model, &query,
					    orm_scope_call(&scope), &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return NB_ERR_NOT_FOUND;

	const struct hook_trust_row *row = norm_rows_at(rows, 0);

	record->digest = orm_text_copy(row->digest);
	record->definition = orm_text_copy(row->definition);
	record->disabled = row->disabled;
	if (!record->digest || !record->definition) {
		hook_store_trust_cleanup(record);
		return NB_ERR_NOMEM;
	}

	return 0;
}

int hook_store_workspace_trusted(struct db *db, const char *path)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	struct norm_select_query query = {
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
		.where = norm_eq(norm_col(&hook_workspace_model, 0), orm_string(path))};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(db,
				norm_select(scope.connection, &hook_workspace_model, &query,
					    orm_scope_call(&scope), &rows, &error),
				&error);

	return rc == 0 ? (norm_rows_count(rows) ? 1 : 0) : rc;
}

int hook_store_workspace_set(struct db *db, const char *path, int trusted)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	int rc = orm_scope_begin(db, &scope);

	if (rc != 0)
		return rc;

	if (trusted) {
		const struct hook_workspace_row row = {.path = nb_str_from_cstr(path)};
		const struct norm_conflict conflict = {.action = NORM_CONFLICT_NOTHING};
		const struct norm_insert_options options = {.conflict = &conflict};

		return orm_status(db,
				  norm_insert(scope.connection, &hook_workspace_model, &row,
					      &options, orm_scope_call(&scope), NULL, &error),
				  &error);
	}

	const struct norm_delete_query query = {
		.where = norm_eq(norm_col(&hook_workspace_model, 0), orm_string(path))};

	return orm_status(db,
			  norm_delete(scope.connection, &hook_workspace_model, &query,
				      orm_scope_call(&scope), NULL, &error),
			  &error);
}

int hook_store_migrate_legacy(struct db *db, const char *id, const char *digest,
			      const char *legacy_id, const char *legacy_digest)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_table_ref legacy = norm_table(&hook_trust_model, NB_STR_LIT("legacy"));
	const struct norm_term new_digest = norm_term_value(orm_string(digest));
	const struct norm_term empty = norm_term_value(orm_string(""));
	const struct norm_expr digest_matches =
		norm_eq(norm_table_col(&legacy, 1), orm_string(legacy_digest));
	const struct norm_case translated = {.when = digest_matches, .then = &new_digest};
	const struct norm_expr eligible[] = {
		digest_matches,
		norm_eq(norm_table_col(&legacy, 2), norm_i32(1)),
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&legacy, 0), orm_string(legacy_id)),
		norm_or(eligible, NB_ARRAY_SIZE(eligible)),
	};
	const struct norm_query_field selected[] = {
		{norm_term_value(orm_string(id)), 0},
		{norm_term_case(&translated, 1, &empty), 1},
		{norm_term_column(norm_table_col(&legacy, 2)), 2},
		{norm_term_column(norm_table_col(&legacy, 3)), 3},
	};
	const struct norm_query query = {
		.from = &legacy,
		.projection = &hook_trust_model,
		.fields = selected,
		.field_count = NB_ARRAY_SIZE(selected),
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
	};
	const size_t fields[] = {0, 1, 2, 3};
	const struct norm_conflict conflict = {.action = NORM_CONFLICT_NOTHING};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_insert_select(scope.connection, &hook_trust_model, fields,
						  NB_ARRAY_SIZE(fields), &query, &conflict,
						  orm_scope_call(&scope), NULL, &error),
			       &error);
}

int hook_store_action(struct db *db, const char *id, const char *action, const char *digest,
		      const char *definition)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	bool trust = !strcmp(action, "trust");
	bool revoke = !strcmp(action, "revoke");
	bool disable = !strcmp(action, "disable");
	bool enable = !strcmp(action, "enable");

	if (!trust && !revoke && !disable && !enable)
		return NB_ERR_INVAL;

	int rc = orm_scope_begin(db, &scope);

	if (rc != 0)
		return rc;

	if (enable) {
		const struct norm_assignment change = {.column = norm_col(&hook_trust_model, 2),
						       .value = norm_i32(0)};
		const struct norm_update_query query = {
			.set = &change,
			.set_count = 1,
			.where = norm_eq(norm_col(&hook_trust_model, 0), orm_string(id)),
		};

		return orm_status(db,
				  norm_update(scope.connection, &hook_trust_model, &query,
					      orm_scope_call(&scope), NULL, &error),
				  &error);
	}

	const struct hook_trust_row row = {
		.id = nb_str_from_cstr(id),
		.digest = nb_str_from_cstr(trust ? digest : ""),
		.definition = nb_str_from_cstr(trust ? definition : ""),
		.disabled = disable,
	};
	const size_t key = 0;
	const size_t updated[] = {2, 1, 3};
	struct norm_term excluded[NB_ARRAY_SIZE(updated)] = {0};
	struct norm_assignment changes[NB_ARRAY_SIZE(updated)] = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(updated); i++) {
		excluded[i] = norm_term_excluded(norm_col(&hook_trust_model, updated[i]));
		changes[i] =
			(struct norm_assignment){.column = norm_col(&hook_trust_model, updated[i]),
						 .expression = &excluded[i]};
	}

	const struct norm_conflict conflict = {
		.action = NORM_CONFLICT_UPDATE,
		.fields = &key,
		.field_count = 1,
		.set = changes,
		.set_count = disable ? 1 : NB_ARRAY_SIZE(changes),
	};
	const struct norm_insert_options options = {.conflict = &conflict};

	return orm_status(db,
			  norm_insert(scope.connection, &hook_trust_model, &row, &options,
				      orm_scope_call(&scope), NULL, &error),
			  &error);
}

int hook_store_batch_write(struct db *db, const char *workspace, int trust,
			   const struct store_hook_choice *choices, size_t count)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0 && trust)
		rc = hook_store_workspace_set(db, workspace, 1);

	for (size_t i = 0; rc == 0 && i < count; i++)
		rc = hook_store_action(db, choices[i].id, trust ? "trust" : "disable",
				       choices[i].digest, choices[i].definition);

	return rc;
}
