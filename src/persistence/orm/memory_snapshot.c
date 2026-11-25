#include "memory_snapshot.h"
#include "memory_models.h"
#include "records.h"
#include "errors.h"
#include <norm/query.h>

static char *snapshot_text(nb_str text, bool missing, int *error)
{
	if (missing)
		return NULL;

	char *copy = orm_text_copy(text);

	if (!copy)
		*error = NB_ERR_NOMEM;

	return copy;
}

static int snapshot_select(struct db *db, const struct norm_model *model,
			   const struct norm_select_query *query, struct norm_rows **rows)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	int rc = orm_scope_begin(db, &lease);

	return rc ? rc
		  : orm_status(db,
			       norm_select(lease.connection, model, query, orm_scope_call(&lease),
					   rows, &error),
			       &error);
}

static int snapshot_facts(struct db *db, int64_t session_id,
			  const struct memory_snapshot_options *options,
			  struct memory_snapshot *snapshot)
{
	const struct norm_value aliases[] = {
		orm_string("preferred_language"),
		orm_string("response_style"),
		orm_string("preferred_name"),
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&fact_model, FACT_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_col(&fact_model, FACT_IS_CURRENT), norm_i32(1)),
		norm_ne(norm_col(&fact_model, FACT_CATEGORY), orm_string("preference")),
		norm_not_in(norm_col(&fact_model, FACT_KEY_NAME), aliases, NB_ARRAY_SIZE(aliases)),
	};
	const size_t fields[] = {FACT_KEY_NAME, FACT_VALUE_TEXT, FACT_CATEGORY, FACT_IMPORTANCE};
	const struct norm_order recent[] = {
		{.column = norm_col(&fact_model, FACT_UPDATED_AT), .direction = NORM_DESC}};
	const struct norm_order important[] = {
		{.column = norm_col(&fact_model, FACT_IMPORTANCE), .direction = NORM_DESC},
		{.column = norm_col(&fact_model, FACT_UPDATED_AT), .direction = NORM_DESC},
	};
	struct norm_select_query query = {
		.where = norm_and(conditions, options->for_reply ? 4 : 2),
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.order = options->for_reply || options->facts_by_recency ? recent : important,
		.order_count = options->for_reply || options->facts_by_recency ? 1 : 2,
		.has_limit = options->facts_limit > 0,
		.limit = options->facts_limit > 0 ? (uint64_t)options->facts_limit : 0,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	int rc = snapshot_select(db, &fact_model, &query, &rows);
	int error = 0;

	for (size_t i = 0; rc == 0 && !error && i < norm_rows_count(rows); i++) {
		const struct fact_row *row = norm_rows_at(rows, i);
		struct memory_fact_record *record = nb_vec_push(&snapshot->facts);

		if (!record)
			return NB_ERR_NOMEM;

		memset(record, 0, sizeof(*record));
		record->key = snapshot_text(row->key_name, false, &error);
		record->value = snapshot_text(row->value_text, false, &error);
		record->category = snapshot_text(row->category, row->category_missing, &error);
		record->importance = row->importance;
	}

	return rc ? rc : error;
}

static int snapshot_rules(struct db *db, int64_t session_id, struct memory_snapshot *snapshot)
{
	const size_t fields[] = {PROCEDURE_RULE_TEXT, PROCEDURE_EVIDENCE_COUNT};
	const struct norm_order order[] = {
		{.column = norm_col(&memory_procedure_model, PROCEDURE_EVIDENCE_COUNT),
		 .direction = NORM_DESC},
		{.column = norm_col(&memory_procedure_model, PROCEDURE_UPDATED_AT),
		 .direction = NORM_DESC},
	};
	const struct norm_select_query query = {
		.where = norm_eq(norm_col(&memory_procedure_model, PROCEDURE_SESSION_ID),
				 norm_i64(session_id)),
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.order = order,
		.order_count = NB_ARRAY_SIZE(order),
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	int rc = snapshot_select(db, &memory_procedure_model, &query, &rows);
	int error = 0;

	for (size_t i = 0; rc == 0 && !error && i < norm_rows_count(rows); i++) {
		const struct memory_procedure_row *row = norm_rows_at(rows, i);
		struct memory_rule_record *record = nb_vec_push(&snapshot->rules);

		if (!record)
			return NB_ERR_NOMEM;

		memset(record, 0, sizeof(*record));
		record->text = snapshot_text(row->rule_text, false, &error);
		record->evidence = row->evidence_count;
	}

	return rc ? rc : error;
}

static int snapshot_tasks(struct db *db, int64_t session_id, int limit,
			  struct memory_snapshot *snapshot)
{
	const size_t fields[] = {EPISODE_SUMMARY_TEXT, EPISODE_KEY_DECISIONS, EPISODE_ARTIFACTS,
				 EPISODE_TOOLS_USED,   EPISODE_IMPORTANCE,    EPISODE_CREATED_AT};
	const struct norm_order order = {
		.column = norm_col(&memory_episode_model, EPISODE_CREATED_AT),
		.direction = NORM_DESC};
	const struct norm_select_query query = {
		.where = norm_eq(norm_col(&memory_episode_model, EPISODE_SESSION_ID),
				 norm_i64(session_id)),
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.order = &order,
		.order_count = 1,
		.has_limit = limit > 0,
		.limit = limit > 0 ? (uint64_t)limit : 0,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	int rc = snapshot_select(db, &memory_episode_model, &query, &rows);
	int error = 0;

	for (size_t i = 0; rc == 0 && !error && i < norm_rows_count(rows); i++) {
		const struct memory_episode_row *row = norm_rows_at(rows, i);
		struct memory_task_record *record = nb_vec_push(&snapshot->tasks);

		if (!record)
			return NB_ERR_NOMEM;

		memset(record, 0, sizeof(*record));
		record->summary = snapshot_text(row->summary_text, false, &error);
		record->decisions =
			snapshot_text(row->key_decisions, row->key_decisions_missing, &error);
		record->artifacts = snapshot_text(row->artifacts, row->artifacts_missing, &error);
		record->tools = snapshot_text(row->tools_used, row->tools_used_missing, &error);
		record->importance = row->importance;
		record->created_at = row->created_at;
	}

	if (rc != 0 || error)
		return rc ? rc : error;

	orm_rows_cleanup(&rows);
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_table_ref source = norm_table(&memory_episode_model, NB_STR_LIT("episode"));
	const struct norm_query_field field = {
		.value = norm_term_call(NORM_FUNCTION_COUNT, NULL, 0), .into = 0};
	const struct norm_query count = {
		.from = &source,
		.projection = &orm_integer_model,
		.fields = &field,
		.field_count = 1,
		.where = norm_eq(norm_table_col(&source, EPISODE_SESSION_ID), norm_i64(session_id)),
	};
	struct norm_error failure = {0};

	rc = orm_scope_begin(db, &lease);
	if (rc == 0)
		rc = orm_status(db,
				norm_query_select(lease.connection, &count, orm_scope_call(&lease),
						  &rows, &failure),
				&failure);

	if (rc == 0 && norm_rows_count(rows))
		snapshot->task_count =
			(int)((const struct orm_integer *)norm_rows_at(rows, 0))->value;

	return rc;
}

struct snapshot_change_row {
	nb_str key_name;
	bool key_name_missing;
	nb_str old_value;
	bool old_value_missing;
	nb_str new_value;
	bool new_value_missing;
	int64_t updated_at;
	bool updated_at_missing;
	int64_t valid_to;
	bool valid_to_missing;
};
static const struct norm_field change_fields[] = {
	NORM_NULLABLE_FIELD(struct snapshot_change_row, key_name, key_name_missing, NORM_TYPE_TEXT,
			    0),
	NORM_NULLABLE_FIELD(struct snapshot_change_row, old_value, old_value_missing,
			    NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct snapshot_change_row, new_value, new_value_missing,
			    NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct snapshot_change_row, updated_at, updated_at_missing,
			    NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct snapshot_change_row, valid_to, valid_to_missing, NORM_TYPE_I64,
			    0),
};
static const struct norm_model change_model =
	NORM_MODEL(struct snapshot_change_row, "", change_fields);

static int snapshot_changes(struct db *db, int64_t session_id, int limit,
			    struct memory_snapshot *snapshot)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_table_ref old = norm_table(&fact_model, NB_STR_LIT("old"));
	struct norm_table_ref current = norm_table(&fact_model, NB_STR_LIT("current"));
	const struct norm_join join = {
		.kind = NORM_JOIN_LEFT,
		.table = &current,
		.on = norm_columns_eq(norm_table_col(&current, FACT_ID),
				      norm_table_col(&old, FACT_SUPERSEDED_BY)),
	};
	const struct norm_join_column columns[] = {
		{.column = norm_table_col(&old, FACT_KEY_NAME), .into = 0},
		{.column = norm_table_col(&old, FACT_VALUE_TEXT), .into = 1},
		{.column = norm_table_col(&current, FACT_VALUE_TEXT), .into = 2},
		{.column = norm_table_col(&old, FACT_UPDATED_AT), .into = 3},
		{.column = norm_table_col(&old, FACT_VALID_TO), .into = 4},
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&old, FACT_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_table_col(&old, FACT_IS_CURRENT), norm_i32(0)),
	};
	const struct norm_order order = {.column = norm_table_col(&old, FACT_UPDATED_AT),
					 .direction = NORM_DESC};
	const struct norm_join_query query = {
		.from = &old,
		.joins = &join,
		.join_count = 1,
		.projection = &change_model,
		.columns = columns,
		.column_count = NB_ARRAY_SIZE(columns),
		.select = {.where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
			   .order = &order,
			   .order_count = 1,
			   .has_limit = limit > 0,
			   .limit = limit > 0 ? (uint64_t)limit : 0,
			   .max_rows = SIZE_MAX,
			   .max_bytes = SIZE_MAX},
	};
	struct norm_error failure = {0};
	int rc = orm_scope_begin(db, &lease);

	if (rc == 0)
		rc = orm_status(db,
				norm_join_select(lease.connection, &query, orm_scope_call(&lease),
						 &rows, &failure),
				&failure);

	int error = 0;
	for (size_t i = 0; rc == 0 && !error && i < norm_rows_count(rows); i++) {
		const struct snapshot_change_row *row = norm_rows_at(rows, i);
		struct memory_change_record *record = nb_vec_push(&snapshot->changes);

		if (!record)
			return NB_ERR_NOMEM;

		memset(record, 0, sizeof(*record));
		record->key = snapshot_text(row->key_name, row->key_name_missing, &error);
		record->old_value = snapshot_text(row->old_value, row->old_value_missing, &error);
		record->new_value = snapshot_text(row->new_value, row->new_value_missing, &error);
		record->updated_at = row->updated_at;
		record->valid_to = row->valid_to;
	}

	return rc ? rc : error;
}

static int snapshot_user_summary(struct db *db, int64_t session_id,
				 struct memory_snapshot *snapshot)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	const size_t field = PROFILE_PROFILE_TEXT;
	const struct norm_select_query query = {
		.where = norm_eq(norm_col(&memory_profile_model, PROFILE_SESSION_ID),
				 norm_i64(session_id)),
		.fields = &field,
		.field_count = 1,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};
	int rc = snapshot_select(db, &memory_profile_model, &query, &rows);
	int error = 0;

	if (rc == 0 && norm_rows_count(rows)) {
		const struct memory_profile_row *row = norm_rows_at(rows, 0);

		snapshot->user_summary = snapshot_text(row->profile_text, false, &error);
	}

	return rc ? rc : error;
}

void memory_snapshot_cleanup(struct memory_snapshot *snapshot)
{
	if (!snapshot)
		return;

	for (size_t i = 0; i < snapshot->facts.len; i++) {
		struct memory_fact_record *record = nb_vec_at(&snapshot->facts, i);
		free(record->key);
		free(record->value);
		free(record->category);
	}

	for (size_t i = 0; i < snapshot->rules.len; i++) {
		struct memory_rule_record *record = nb_vec_at(&snapshot->rules, i);
		free(record->text);
	}

	for (size_t i = 0; i < snapshot->tasks.len; i++) {
		struct memory_task_record *record = nb_vec_at(&snapshot->tasks, i);
		free(record->summary);
		free(record->decisions);
		free(record->artifacts);
		free(record->tools);
	}

	for (size_t i = 0; i < snapshot->changes.len; i++) {
		struct memory_change_record *record = nb_vec_at(&snapshot->changes, i);
		free(record->key);
		free(record->old_value);
		free(record->new_value);
	}

	free(snapshot->user_summary);
	nb_vec_cleanup(&snapshot->facts);
	nb_vec_cleanup(&snapshot->rules);
	nb_vec_cleanup(&snapshot->tasks);
	nb_vec_cleanup(&snapshot->changes);
	memset(snapshot, 0, sizeof(*snapshot));
}

static void memory_snapshot_move(struct memory_snapshot *destination,
				 struct memory_snapshot *source)
{
	*destination = *source;
	memset(source, 0, sizeof(*source));
}

int memory_snapshot_read(struct db *db, int64_t session_id,
			 const struct memory_snapshot_options *options, struct memory_snapshot *out)
{
	struct memory_snapshot value NB_CLEANUP(memory_snapshot_cleanup) = {0};
	struct memory_fact_record fact;
	struct memory_rule_record rule;
	struct memory_task_record task;
	struct memory_change_record change;
	int rc;

	if (!db_is_open(db) || !options || !out)
		return NB_ERR_INVAL;

	if (out->user_summary || ((void *)out->facts.data) || ((void *)out->rules.data) ||
	    ((void *)out->tasks.data) || ((void *)out->changes.data))
		return NB_ERR_EXISTS;

	rc = (nb_vec_init(&value.facts, sizeof(fact)),
	      nb_vec_reserve(&value.facts, 8) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_vec_init(&value.rules, sizeof(rule)),
		      nb_vec_reserve(&value.rules, 8) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_vec_init(&value.tasks, sizeof(task)),
		      nb_vec_reserve(&value.tasks, 8) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_vec_init(&value.changes, sizeof(change)),
		      nb_vec_reserve(&value.changes, 8) ? 0 : NB_ERR_NOMEM);
	if (rc == 0 && options->facts_limit)
		rc = snapshot_facts(db, session_id, options, &value);

	if (rc == 0 && options->include_rules)
		rc = snapshot_rules(db, session_id, &value);

	if (rc == 0 && options->tasks_limit)
		rc = snapshot_tasks(db, session_id, options->tasks_limit, &value);

	if (rc == 0 && options->changes_limit)
		rc = snapshot_changes(db, session_id, options->changes_limit, &value);

	if (rc == 0 && options->include_user_summary)
		rc = snapshot_user_summary(db, session_id, &value);

	if (rc == 0)
		memory_snapshot_move(out, &value);

	return rc;
}

int memory_store_session_has_rows(struct db *db, int64_t session_id)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	const struct norm_model *const models[] = {&memory_profile_model, &fact_model,
						   &memory_procedure_model, &memory_episode_model,
						   &memory_scope_model};
	const size_t session_fields[] = {PROFILE_SESSION_ID, FACT_SESSION_ID, PROCEDURE_SESSION_ID,
					 EPISODE_SESSION_ID, SCOPE_SESSION_ID};
	struct norm_table_ref sources[NB_ARRAY_SIZE(models)] = {0};
	struct norm_query queries[NB_ARRAY_SIZE(models)] = {0};
	struct norm_query_field fields[NB_ARRAY_SIZE(models)] = {0};
	struct norm_expr conditions[NB_ARRAY_SIZE(models)] = {0};
	struct norm_query_field one = {.value = norm_term_value(norm_i32(1)), .into = 0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(models); i++) {
		sources[i] = norm_table(models[i], NB_STR_LIT("record"));
		fields[i].value = norm_term_column(norm_table_col(&sources[i], session_fields[i]));
		queries[i].from = &sources[i];
		queries[i].projection = &orm_integer_model;
		queries[i].fields = &fields[i];
		queries[i].field_count = 1;
		queries[i].where = norm_eq(norm_table_col(&sources[i], session_fields[i]),
					   norm_i64(session_id));
		conditions[i] = norm_exists(&queries[i]);
	}

	const struct norm_query query = {.projection = &orm_integer_model,
					 .fields = &one,
					 .field_count = 1,
					 .where = norm_or(conditions, NB_ARRAY_SIZE(conditions))};
	struct norm_error failure = {0};
	int rc = orm_scope_begin(db, &lease);

	if (rc == 0)
		rc = orm_status(db,
				norm_query_select(lease.connection, &query, orm_scope_call(&lease),
						  &rows, &failure),
				&failure);

	return rc == 0 && norm_rows_count(rows) > 0;
}
