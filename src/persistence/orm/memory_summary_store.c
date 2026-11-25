#include "memory_summary_store.h"
#include "memory_repository.h"
#include "memory_models.h"
#include "records.h"
#include "errors.h"
#include <time.h>

int memory_summary_revision(struct db *db, int64_t session_id, int64_t *revision)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!revision)
		return NB_ERR_INVAL;

	const size_t field = SCOPE_CONTENT_REVISION;
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

	*revision = ((const struct memory_scope_row *)norm_rows_at(rows, 0))->content_revision;
	return 0;
}

int memory_summary_save(struct db *db, int64_t session_id, int64_t version, int64_t revision,
			const char *title, const char *text, const char *sources)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	int64_t current = 0;
	int rc = orm_scope_begin(db, &lease);

	if (rc == 0)
		rc = memory_summary_revision(db, session_id, &current);

	if (rc != 0)
		return rc;

	if (revision != current)
		return NB_ERR_STALE;

	if (!title || !text || !sources)
		return MORPH_ERR_DB;

	const struct memory_summary_row row = {.session_id = session_id,
					       .memory_version = version,
					       .content_revision = revision,
					       .title = nb_str_from_cstr(title),
					       .text = nb_str_from_cstr(text),
					       .sources = nb_str_from_cstr(sources),
					       .updated_at = (int64_t)time(NULL)};
	const size_t key = SUMMARY_SESSION_ID;
	const size_t copied[] = {SUMMARY_MEMORY_VERSION, SUMMARY_CONTENT_REVISION,
				 SUMMARY_TITLE,		 SUMMARY_TEXT,
				 SUMMARY_SOURCES,	 SUMMARY_UPDATED_AT};
	struct norm_term values[NB_ARRAY_SIZE(copied)] = {0};
	struct norm_assignment assignments[NB_ARRAY_SIZE(copied)] = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(copied); i++) {
		values[i] = norm_term_excluded(norm_col(&memory_summary_model, copied[i]));
		assignments[i].column = norm_col(&memory_summary_model, copied[i]);
		assignments[i].expression = &values[i];
	}

	const struct norm_conflict conflict = {.action = NORM_CONFLICT_UPDATE,
					       .fields = &key,
					       .field_count = 1,
					       .set = assignments,
					       .set_count = NB_ARRAY_SIZE(assignments)};
	const struct norm_insert_options options = {.conflict = &conflict};

	return orm_insert_row(&lease, &memory_summary_model, &row, &options, NULL);
}

int memory_note_save(struct db *db, int64_t session_id, const char *title, const char *text,
		     const char *source, const char *event_token, const char *scope)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_write_result result = {0};

	if (!title || !text || !source || !event_token || !scope ||
	    (strcmp(scope, "project") && strcmp(scope, "session")))
		return NB_ERR_INVAL;

	const struct memory_note_row row = {.session_id = session_id,
					    .title = nb_str_from_cstr(title),
					    .text = nb_str_from_cstr(text),
					    .source = nb_str_from_cstr(source),
					    .event_token = nb_str_from_cstr(event_token),
					    .scope = nb_str_from_cstr(scope),
					    .created_at = (int64_t)time(NULL),
					    .updated_at = (int64_t)time(NULL)};
	const size_t keys[] = {NOTE_SESSION_ID, NOTE_EVENT_TOKEN, NOTE_TITLE};
	const struct norm_conflict conflict = {.action = NORM_CONFLICT_NOTHING,
					       .fields = keys,
					       .field_count = NB_ARRAY_SIZE(keys)};
	const struct norm_insert_options options = {.conflict = &conflict};
	int rc = orm_scope_begin(db, &lease);

	if (rc == 0)
		rc = orm_insert_row(&lease, &memory_note_model, &row, &options, &result);

	if (rc != 0 || result.affected_rows == 1)
		return rc;

	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&memory_note_model, NOTE_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_col(&memory_note_model, NOTE_EVENT_TOKEN), orm_string(event_token)),
		norm_eq(norm_col(&memory_note_model, NOTE_TITLE), orm_string(title)),
		norm_eq(norm_col(&memory_note_model, NOTE_TEXT), orm_string(text)),
		norm_eq(norm_col(&memory_note_model, NOTE_SOURCE), orm_string(source)),
		norm_eq(norm_col(&memory_note_model, NOTE_SCOPE), orm_string(scope)),
	};
	const size_t field = NOTE_ID;
	const struct norm_select_query query = {
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
		.fields = &field,
		.field_count = 1,
		.has_limit = true,
		.limit = 1};

	rc = orm_select_rows(&lease, &memory_note_model, &query, &rows);
	return rc ? rc : norm_rows_count(rows) ? 0 : NB_ERR_STATE;
}

static int memory_record_change(struct db *db, int64_t session_id, const char *kind, int64_t id,
				const char *expected, const char *replacement, const char *source)
{
	const struct norm_model *model = NULL;
	size_t id_field = 0;
	size_t session_field = 1;
	size_t value_field;
	size_t source_field = SIZE_MAX;
	size_t time_field = SIZE_MAX;
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_write_result result = {0};

	if (!strcmp(kind, "notes")) {
		model = &memory_note_model;
		value_field = NOTE_TEXT;
		source_field = NOTE_SOURCE;
		time_field = NOTE_UPDATED_AT;
	} else if (!strcmp(kind, "facts")) {
		model = &fact_model;
		value_field = FACT_VALUE_TEXT;
		source_field = FACT_SOURCE_TEXT;
		time_field = FACT_UPDATED_AT;
	} else if (!strcmp(kind, "learned_facts") || !strcmp(kind, "tips") ||
		   !strcmp(kind, "unapplied_settings")) {
		model = &memory_candidate_model;
		value_field = CANDIDATE_VALUE;
		source_field = CANDIDATE_SOURCE;
		time_field = CANDIDATE_CREATED_AT;
	} else if (!strcmp(kind, "past_tasks") && !replacement) {
		model = &memory_episode_model;
		value_field = EPISODE_SUMMARY_TEXT;
	} else if (!strcmp(kind, "unapplied_rules") && !replacement) {
		model = &memory_procedure_model;
		value_field = PROCEDURE_RULE_TEXT;
	} else {
		return NB_ERR_INVAL;
	}

	const struct norm_expr conditions[] = {
		norm_eq(norm_col(model, id_field), norm_i64(id)),
		norm_eq(norm_col(model, session_field), norm_i64(session_id)),
		norm_eq(norm_col(model, value_field), orm_string(expected)),
	};
	const struct norm_expr where = norm_and(conditions, NB_ARRAY_SIZE(conditions));
	int rc = orm_scope_begin(db, &lease);

	if (rc == 0 && replacement) {
		const struct norm_assignment assignments[] = {
			{.column = norm_col(model, value_field), .value = orm_string(replacement)},
			{.column = norm_col(model, source_field), .value = orm_string(source)},
			{.column = norm_col(model, time_field),
			 .value = norm_i64((int64_t)time(NULL))},
		};

		rc = orm_update_rows(&lease, model, assignments, NB_ARRAY_SIZE(assignments), where,
				     &result);
	} else if (rc == 0) {
		rc = orm_delete_rows(&lease, model, where, &result);
	}

	return rc ? rc : result.affected_rows == 1 ? 0 : NB_ERR_STALE;
}

int memory_record_edit(struct db *db, int64_t session_id, const char *kind, int64_t id,
		       int64_t version, const char *expected_value, const char *replacement,
		       const char *source)
{
	if (!kind || !expected_value || !source || id <= 0 || version < 0)
		return NB_ERR_INVAL;

	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	int rc = db_lock_acquire(db, &lock);

	if (rc != 0)
		return rc;

	rc = memory_store_begin_versioned_write(db, session_id, version);
	if (rc != 0)
		return rc;

	rc = memory_record_change(db, session_id, kind, id, expected_value, replacement, source);
	if (rc == 0) {
		struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
		const struct norm_term generation =
			norm_term_column(norm_col(&memory_scope_model, SCOPE_GENERATION));
		const struct norm_term one = norm_term_value(norm_i64(1));
		const struct norm_term next = norm_term_binary(NORM_BINARY_ADD, &generation, &one);
		const struct norm_assignment assignment = {
			.column = norm_col(&memory_scope_model, SCOPE_GENERATION),
			.expression = &next};

		rc = orm_scope_begin(db, &lease);
		if (rc == 0)
			rc = orm_update_rows(
				&lease, &memory_scope_model, &assignment, 1,
				norm_eq(norm_col(&memory_scope_model, SCOPE_SESSION_ID),
					norm_i64(session_id)),
				NULL);
	}

	return memory_store_finish_write(db, rc);
}
