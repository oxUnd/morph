#include "memory_repository.h"
#include "preferences.h"
#include "records.h"
#include "memory_models.h"
#include "history_models.h"
#include "errors.h"
#include <stdio.h>
#include <time.h>

int memory_store_begin_write(struct db *db)
{
	int rc = orm_write_begin(db, NORM_SQLITE_IMMEDIATE);

	return rc == NB_ERR_STATE ? MORPH_ERR_DB : rc;
}

int memory_store_finish_write(struct db *db, int result)
{
	return orm_write_finish(db, result);
}

static int fact_update(struct orm_scope *lease, int64_t id,
		       const struct norm_assignment *assignments, size_t count)
{
	struct norm_error error = {0};
	struct norm_update_query query = {
		.set = assignments,
		.set_count = count,
		.where = norm_eq(norm_col(&fact_model, 0), norm_i64(id)),
	};

	int rc = orm_status(lease->database,
			    norm_update(lease->connection, &fact_model, &query,
					orm_scope_call(lease), NULL, &error),
			    &error);

	return rc == MORPH_ERR_DB ? NB_ERR_IO : rc;
}

int memory_store_write_fact(struct db *db, int64_t session_id, const char *key_name,
			    const char *value_text, const char *source_text, const char *category,
			    double importance)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};

	if (!key_name || !value_text || !*value_text)
		return NB_ERR_INVAL;

	if (!category || !*category)
		category = "general";

	if (importance < 0.0 || importance > 1.0)
		importance = 0.5;

	int rc = orm_scope_begin(db, &lease);
	struct norm_expr conditions[] = {
		norm_eq(norm_col(&fact_model, 1), norm_i64(session_id)),
		norm_eq(norm_col(&fact_model, 2), orm_string(key_name)),
		norm_eq(norm_col(&fact_model, 8), norm_i32(1)),
	};
	struct norm_order order = {.column = norm_col(&fact_model, 13), .direction = NORM_DESC};
	const size_t fields[] = {0, 3};
	struct norm_select_query query = {
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
		.order = &order,
		.order_count = 1,
		.has_limit = true,
		.limit = 1,
	};

	if (rc == 0)
		rc = orm_status(db,
				norm_select(lease.connection, &fact_model, &query,
					    orm_scope_call(&lease), &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	const struct fact_row *previous = norm_rows_count(rows) ? norm_rows_at(rows, 0) : NULL;
	int64_t previous_id = previous ? previous->id : 0;
	int64_t now = (int64_t)time(NULL);

	if (previous && nb_str_eq_cstr(previous->value_text, value_text)) {
		struct norm_assignment changes[] = {
			{.column = norm_col(&fact_model, 4),
			 .value = orm_string(source_text ? source_text : "")},
			{.column = norm_col(&fact_model, 13), .value = norm_i64(now)},
			{.column = norm_col(&fact_model, 10), .value = norm_null()},
			{.column = norm_col(&fact_model, 8), .value = norm_i32(1)},
			{.column = norm_col(&fact_model, 6), .value = orm_string(category)},
			{.column = norm_col(&fact_model, 7), .value = norm_f64(importance)},
		};

		return fact_update(&lease, previous_id, changes, NB_ARRAY_SIZE(changes));
	}

	if (previous_id > 0) {
		struct norm_assignment changes[] = {
			{.column = norm_col(&fact_model, 8), .value = norm_i32(0)},
			{.column = norm_col(&fact_model, 10), .value = norm_i64(now)},
			{.column = norm_col(&fact_model, 13), .value = norm_i64(now)},
		};

		rc = fact_update(&lease, previous_id, changes, NB_ARRAY_SIZE(changes));
		if (rc != 0)
			return rc;
	}

	struct fact_row row = {
		.session_id = session_id,
		.key_name = nb_str_from_cstr(key_name),
		.value_text = nb_str_from_cstr(value_text),
		.source_text = nb_str_from_cstr(source_text ? source_text : ""),
		.confidence = 1.0,
		.category = nb_str_from_cstr(category),
		.importance = importance,
		.is_current = 1,
		.valid_from = now,
		.valid_to_missing = true,
		.superseded_by_missing = true,
		.created_at = now,
		.updated_at = now,
	};
	struct norm_write_result result = {0};

	rc = orm_status(db,
			norm_insert(lease.connection, &fact_model, &row, NULL,
				    orm_scope_call(&lease), &result, &error),
			&error);
	if (rc == MORPH_ERR_DB)
		rc = NB_ERR_IO;

	if (rc == 0 && previous_id > 0) {
		struct norm_assignment link = {.column = norm_col(&fact_model, 11),
					       .value = norm_i64(result.insert_id)};

		rc = result.has_insert_id ? fact_update(&lease, previous_id, &link, 1)
					  : MORPH_ERR_DB;
	}

	return rc;
}

int memory_store_upsert_fact(struct db *db, int64_t session_id, const char *key_name,
			     const char *value_text, const char *source_text, const char *category,
			     double importance)
{
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	int rc = db_lock_acquire(db, &lock);

	if (rc != 0)
		return rc;

	rc = memory_store_begin_write(db);
	if (rc != 0)
		return rc;

	rc = memory_store_write_fact(db, session_id, key_name, value_text, source_text, category,
				     importance);
	return memory_store_finish_write(db, rc);
}

int memory_store_prepare_session(struct db *db, int64_t session_id)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	const size_t field = SCOPE_SESSION_ID;
	const struct norm_select_query query = {
		.fields = &field,
		.field_count = 1,
		.where = norm_eq(norm_col(&memory_scope_model, SCOPE_SESSION_ID),
				 norm_i64(session_id)),
		.has_limit = true,
		.limit = 1};
	int rc = orm_scope_begin(db, &lease);

	if (rc == 0)
		rc = orm_select_rows(&lease, &memory_scope_model, &query, &rows);

	return rc ? rc : norm_rows_count(rows) ? 0 : preference_bind(db, session_id, "local", "");
}

int memory_store_read_version(struct db *db, int64_t session_id, int64_t *memory_version)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!memory_version)
		return NB_ERR_INVAL;

	const size_t field = SCOPE_GENERATION;
	const struct norm_select_query query = {
		.fields = &field,
		.field_count = 1,
		.where = norm_eq(norm_col(&memory_scope_model, SCOPE_SESSION_ID),
				 norm_i64(session_id))};
	int rc = orm_scope_begin(db, &lease);

	if (rc == 0)
		rc = orm_select_rows(&lease, &memory_scope_model, &query, &rows);

	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return NB_ERR_NOT_FOUND;

	*memory_version = ((const struct memory_scope_row *)norm_rows_at(rows, 0))->generation;
	return 0;
}

int memory_store_begin_versioned_write(struct db *db, int64_t session_id, int64_t expected)
{
	int64_t current = 0;
	int rc = memory_store_begin_write(db);

	if (rc != 0)
		return rc;

	rc = memory_store_read_version(db, session_id, &current);
	if (rc == 0 && current != expected)
		rc = NB_ERR_STALE;

	if (rc != 0)
		return memory_store_finish_write(db, rc);

	return 0;
}

int memory_store_clear(struct db *db, int64_t session_id, enum memory_clear_scope scope)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};

	if (scope < MEMORY_CLEAR_ALL || scope > MEMORY_CLEAR_TIPS)
		return NB_ERR_INVAL;

	int rc = orm_scope_begin(db, &lease);

	if (rc == 0)
		rc = memory_store_prepare_session(db, session_id);

	if (rc == 0)
		rc = memory_store_begin_write(db);

	if (rc != 0)
		return rc;

	const struct norm_term generation =
		norm_term_column(norm_col(&memory_scope_model, SCOPE_GENERATION));
	const struct norm_term one = norm_term_value(norm_i64(1));
	const struct norm_term next = norm_term_binary(NORM_BINARY_ADD, &generation, &one);
	const struct norm_assignment advance[] = {
		{.column = norm_col(&memory_scope_model, SCOPE_GENERATION), .expression = &next},
		{.column = norm_col(&memory_scope_model, SCOPE_MIGRATED), .value = norm_i32(1)},
	};

	rc = orm_update_rows(
		&lease, &memory_scope_model, advance, NB_ARRAY_SIZE(advance),
		norm_eq(norm_col(&memory_scope_model, SCOPE_SESSION_ID), norm_i64(session_id)),
		NULL);
	if (rc == 0) {
		switch (scope) {
		case MEMORY_CLEAR_ALL:
			rc = preference_clear_session(db, session_id);
			if (rc == 0)
				rc = memory_store_clear_all(db, session_id);

			break;
		case MEMORY_CLEAR_FACTS:
			rc = memory_store_clear_facts(db, session_id);
			break;
		case MEMORY_CLEAR_TASKS:
			rc = memory_store_clear_tasks(db, session_id);
			break;
		case MEMORY_CLEAR_TIPS:
			rc = memory_store_clear_tips(db, session_id);
			break;
		}
	}

	if (rc == 0 && scope == MEMORY_CLEAR_ALL)
		rc = orm_delete_rows(
			&lease, &memory_candidate_model,
			norm_eq(norm_col(&memory_candidate_model, CANDIDATE_SESSION_ID),
				norm_i64(session_id)),
			NULL);

	if (rc == 0)
		rc = orm_delete_rows(&lease, &memory_summary_model,
				     norm_eq(norm_col(&memory_summary_model, SUMMARY_SESSION_ID),
					     norm_i64(session_id)),
				     NULL);

	if (rc == 0) {
		const struct norm_term state = norm_term_column(norm_col(&job_model, JOB_STATE));
		const struct norm_term cancelled = norm_term_value(orm_string("cancelled"));
		const struct norm_case completed = {
			.when = norm_eq(norm_col(&job_model, JOB_STATE), orm_string("completed")),
			.then = &state};
		const struct norm_term updated = norm_term_case(&completed, 1, &cancelled);
		const struct norm_term payload =
			scope == MEMORY_CLEAR_ALL
				? norm_term_value(orm_string("{}"))
				: norm_term_column(norm_col(&job_model, JOB_PAYLOAD));
		const struct norm_assignment assignments[] = {
			{.column = norm_col(&job_model, JOB_STATE), .expression = &updated},
			{.column = norm_col(&job_model, JOB_PAYLOAD), .expression = &payload},
		};

		rc = orm_update_rows(
			&lease, &job_model, assignments, NB_ARRAY_SIZE(assignments),
			norm_eq(norm_col(&job_model, JOB_SESSION_ID), norm_i64(session_id)), NULL);
	}

	return memory_store_finish_write(db, rc);
}

struct legacy_fact_row {
	int64_t id;
	nb_str key_name;
	nb_str value_text;
	nb_str source_text;
};
static const struct norm_field legacy_fact_fields[] = {
	NORM_FIELD(struct legacy_fact_row, id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct legacy_fact_row, key_name, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct legacy_fact_row, value_text, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct legacy_fact_row, source_text, NORM_TYPE_TEXT, 0),
};
static const struct norm_model legacy_fact_model =
	NORM_MODEL(struct legacy_fact_row, "", legacy_fact_fields);

static int migrate_preference(struct orm_scope *lease, int64_t session_id, const char *key,
			      const char *value, const char *source, const char *token)
{
	struct norm_table_ref scope = norm_table(&memory_scope_model, NB_STR_LIT("scope"));
	const struct norm_term id = norm_term_column(norm_table_col(&scope, SCOPE_SESSION_ID));
	const size_t targets[] = {PREFERENCE_OWNER,	 PREFERENCE_SCOPE,	 PREFERENCE_TARGET,
				  PREFERENCE_KEY,	 PREFERENCE_VALUE,	 PREFERENCE_SOURCE,
				  PREFERENCE_SESSION_ID, PREFERENCE_EVENT_TOKEN, PREFERENCE_ORIGIN};
	const struct norm_query_field fields[] = {
		{.value = norm_term_column(norm_table_col(&scope, SCOPE_OWNER)),
		 .into = PREFERENCE_OWNER},
		{.value = norm_term_value(orm_string("session")), .into = PREFERENCE_SCOPE},
		{.value = norm_term_cast(&id, NORM_TYPE_TEXT), .into = PREFERENCE_TARGET},
		{.value = norm_term_value(orm_string(preference_key_borrow(key))),
		 .into = PREFERENCE_KEY},
		{.value = norm_term_value(orm_string(value)), .into = PREFERENCE_VALUE},
		{.value = norm_term_value(orm_string(source)), .into = PREFERENCE_SOURCE},
		{.value = id, .into = PREFERENCE_SESSION_ID},
		{.value = norm_term_value(orm_string(token)), .into = PREFERENCE_EVENT_TOKEN},
		{.value = norm_term_value(orm_string("legacy")), .into = PREFERENCE_ORIGIN},
	};
	const struct norm_query query = {
		.from = &scope,
		.projection = &memory_preference_model,
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.where = norm_eq(norm_table_col(&scope, SCOPE_SESSION_ID), norm_i64(session_id))};
	const struct norm_conflict conflict = {.action = NORM_CONFLICT_NOTHING};

	return orm_insert_from(lease, &memory_preference_model, targets, NB_ARRAY_SIZE(targets),
			       &query, &conflict, NULL);
}

int memory_store_migrate_preferences(struct db *db, int64_t session_id,
				     int (*verify)(const char *key, const char *value,
						   const char *source))
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_table_ref facts = norm_table(&fact_model, NB_STR_LIT("fact"));
	struct norm_table_ref scope = norm_table(&memory_scope_model, NB_STR_LIT("scope"));
	const struct norm_join join = {
		.kind = NORM_JOIN_INNER,
		.table = &scope,
		.on = norm_columns_eq(norm_table_col(&scope, SCOPE_SESSION_ID),
				      norm_table_col(&facts, FACT_SESSION_ID))};
	const struct norm_term source_parts[] = {
		norm_term_column(norm_table_col(&facts, FACT_SOURCE_TEXT)),
		norm_term_value(orm_string(""))};
	const struct norm_query_field fields[] = {
		{.value = norm_term_column(norm_table_col(&facts, FACT_ID)), .into = 0},
		{.value = norm_term_column(norm_table_col(&facts, FACT_KEY_NAME)), .into = 1},
		{.value = norm_term_column(norm_table_col(&facts, FACT_VALUE_TEXT)), .into = 2},
		{.value = norm_term_call(NORM_FUNCTION_COALESCE, source_parts,
					 NB_ARRAY_SIZE(source_parts)),
		 .into = 3},
	};
	const struct norm_value aliases[] = {orm_string("preferred_language"),
					     orm_string("response_style"),
					     orm_string("preferred_name")};
	const struct norm_expr legacy[] = {
		norm_eq(norm_table_col(&facts, FACT_CATEGORY), orm_string("preference")),
		norm_in(norm_table_col(&facts, FACT_KEY_NAME), aliases, NB_ARRAY_SIZE(aliases)),
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&facts, FACT_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_table_col(&scope, SCOPE_MIGRATED), norm_i32(0)),
		norm_eq(norm_table_col(&facts, FACT_IS_CURRENT), norm_i32(1)),
		norm_or(legacy, NB_ARRAY_SIZE(legacy)),
	};
	const struct norm_query_order order = {
		.value = norm_term_column(norm_table_col(&facts, FACT_ID)), .direction = NORM_ASC};
	const struct norm_query query = {.from = &facts,
					 .joins = &join,
					 .join_count = 1,
					 .projection = &legacy_fact_model,
					 .fields = fields,
					 .field_count = NB_ARRAY_SIZE(fields),
					 .where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
					 .order = &order,
					 .order_count = 1};
	int rc = orm_scope_begin(db, &lease);

	if (rc == 0)
		rc = orm_read(&lease, &query, &rows);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct legacy_fact_row *row = norm_rows_at(rows, i);
		char *key NB_CLEANUP(nb_charp_cleanup) = orm_text_copy(row->key_name);
		char *value NB_CLEANUP(nb_charp_cleanup) = orm_text_copy(row->value_text);
		char *source NB_CLEANUP(nb_charp_cleanup) = orm_text_copy(row->source_text);

		if (!key || !value || !source)
			return NB_ERR_NOMEM;

		int verified = verify && verify(key, value, source);

		rc = memory_store_add_note(db, session_id, "legacy", key, value, source);
		if (rc == 0 && verified) {
			char token[64];
			int length =
				snprintf(token, sizeof(token), "legacy:%lld", (long long)row->id);

			if (length < 0 || (size_t)length >= sizeof(token))
				return NB_ERR_NAME_TOO_LONG;

			rc = migrate_preference(&lease, session_id, key, value, source, token);
		}
	}

	const struct norm_assignment migrated = {
		.column = norm_col(&memory_scope_model, SCOPE_MIGRATED), .value = norm_i32(1)};

	return rc ? rc
		  : orm_update_rows(&lease, &memory_scope_model, &migrated, 1,
				    norm_eq(norm_col(&memory_scope_model, SCOPE_SESSION_ID),
					    norm_i64(session_id)),
				    NULL);
}

int memory_store_event_token(struct db *db, int64_t session_id, const char *event_token, char *out,
			     size_t capacity)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!event_token || !out || !capacity)
		return NB_ERR_INVAL;

	const struct norm_expr identity[] = {
		norm_eq(norm_col(&history_model, HIST_TURN_ID), orm_string(event_token)),
		norm_eq(norm_col(&history_model, HIST_IDEMPOTENCY_KEY), orm_string(event_token)),
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&history_model, HIST_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_col(&history_model, HIST_ROLE), orm_string("user")),
		norm_or(identity, NB_ARRAY_SIZE(identity)),
	};
	const size_t field = HIST_ID;
	const struct norm_order order = {.column = norm_col(&history_model, HIST_SEQUENCE_NO),
					 .direction = NORM_DESC};
	const struct norm_select_query query = {
		.fields = &field,
		.field_count = 1,
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
		.order = &order,
		.order_count = 1,
		.has_limit = true,
		.limit = 1};
	int rc = orm_scope_begin(db, &lease);

	if (rc == 0)
		rc = orm_select_rows(&lease, &history_model, &query, &rows);

	if (rc != 0 || !norm_rows_count(rows))
		return rc;

	int bytes = snprintf(out, capacity, "history:%lld",
			     (long long)((const struct history_row *)norm_rows_at(rows, 0))->id);

	return bytes < 0 || (size_t)bytes >= capacity ? NB_ERR_NAME_TOO_LONG : 1;
}

int memory_store_add_task(struct db *db, int64_t session_id, const struct memory_task_input *input)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};

	if (!input || !input->task_type || !input->summary || !input->outcome)
		return NB_ERR_INVAL;

	const struct memory_episode_row row = {
		.session_id = session_id,
		.task_type = nb_str_from_cstr(input->task_type),
		.summary_text = nb_str_from_cstr(input->summary),
		.outcome_text = nb_str_from_cstr(input->outcome),
		.success = !!input->success,
		.entities = nb_str_from_cstr(input->entities ? input->entities : ""),
		.key_decisions = nb_str_from_cstr(input->decisions ? input->decisions : ""),
		.artifacts = nb_str_from_cstr(input->artifacts ? input->artifacts : ""),
		.tools_used = nb_str_from_cstr(input->tools ? input->tools : ""),
		.importance = input->importance,
		.created_at = (int64_t)time(NULL),
		.job_id = input->job_id,
		.job_id_missing = input->job_id <= 0};
	const struct norm_conflict conflict = {.action = NORM_CONFLICT_NOTHING};
	const struct norm_insert_options options = {.conflict = &conflict};
	int rc = orm_scope_begin(db, &lease);

	return rc ? rc : orm_insert_row(&lease, &memory_episode_model, &row, &options, NULL);
}

int memory_store_add_note(struct db *db, int64_t session_id, const char *kind, const char *key,
			  const char *value, const char *source)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};

	if (!kind || !key || !value)
		return MORPH_ERR_DB;

	const struct memory_candidate_row row = {.session_id = session_id,
						 .kind = nb_str_from_cstr(kind),
						 .key = nb_str_from_cstr(key),
						 .value = nb_str_from_cstr(value),
						 .source = nb_str_from_cstr(source ? source : ""),
						 .created_at = (int64_t)time(NULL)};
	const struct norm_conflict conflict = {.action = NORM_CONFLICT_NOTHING};
	const struct norm_insert_options options = {.conflict = &conflict};
	int rc = orm_scope_begin(db, &lease);

	return rc ? rc : orm_insert_row(&lease, &memory_candidate_model, &row, &options, NULL);
}
