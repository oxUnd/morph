#include "memory_store.h"
#include "records.h"
#include "errors.h"

struct memory_scope_row {
	int64_t session_id;
	nb_str kind;
};

static const struct norm_field memory_scope_fields[] = {
	NORM_FIELD(struct memory_scope_row, session_id, NORM_TYPE_I64, 0),
};
static const struct norm_field memory_candidate_fields[] = {
	NORM_FIELD(struct memory_scope_row, session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct memory_scope_row, kind, NORM_TYPE_TEXT, 0),
};
static const struct norm_model notes_model =
	NORM_MODEL(struct memory_scope_row, "memory_notes", memory_scope_fields);
static const struct norm_model profiles_model =
	NORM_MODEL(struct memory_scope_row, "memory_profiles", memory_scope_fields);
static const struct norm_model facts_model =
	NORM_MODEL(struct memory_scope_row, "memory_facts", memory_scope_fields);
static const struct norm_model episodes_model =
	NORM_MODEL(struct memory_scope_row, "memory_episodes", memory_scope_fields);
static const struct norm_model procedures_model =
	NORM_MODEL(struct memory_scope_row, "memory_procedures", memory_scope_fields);
static const struct norm_model candidates_model =
	NORM_MODEL(struct memory_scope_row, "memory_candidates", memory_candidate_fields);

static int memory_store_delete(struct db *db, const struct norm_model *model, int64_t session_id,
			       int candidate_filter)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	struct norm_value kinds[] = {
		orm_string("tip"),	  orm_string("lesson"), orm_string("rule"),
		orm_string("preference"), orm_string("legacy"),
	};
	struct norm_expr conditions[] = {
		norm_eq(norm_col(model, 0), norm_i64(session_id)),
		candidate_filter > 0 ? norm_in(norm_col(model, 1), kinds, 3)
				     : norm_not_in(norm_col(model, 1), kinds, NB_ARRAY_SIZE(kinds)),
	};
	struct norm_delete_query query = {
		.where = candidate_filter ? norm_and(conditions, NB_ARRAY_SIZE(conditions))
					  : conditions[0],
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(db,
				norm_delete(scope.connection, model, &query, orm_scope_call(&scope),
					    NULL, &error),
				&error);

	return rc;
}

int memory_store_clear_all(struct db *db, int64_t session_id)
{
	int rc = memory_store_clear_facts(db, session_id);

	if (rc == 0)
		rc = memory_store_clear_tasks(db, session_id);

	if (rc == 0)
		rc = memory_store_clear_tips(db, session_id);

	return rc;
}

int memory_store_clear_facts(struct db *db, int64_t session_id)
{
	const struct norm_model *const models[] = {&notes_model, &profiles_model, &facts_model};
	int rc = 0;

	for (size_t i = 0; rc == 0 && i < NB_ARRAY_SIZE(models); i++)
		rc = memory_store_delete(db, models[i], session_id, 0);

	if (rc == 0)
		rc = memory_store_delete(db, &candidates_model, session_id, -1);

	return rc;
}

int memory_store_clear_tasks(struct db *db, int64_t session_id)
{
	return memory_store_delete(db, &episodes_model, session_id, 0);
}

int memory_store_clear_tips(struct db *db, int64_t session_id)
{
	int rc = memory_store_delete(db, &procedures_model, session_id, 0);

	if (rc == 0)
		rc = memory_store_delete(db, &candidates_model, session_id, 1);

	return rc;
}
