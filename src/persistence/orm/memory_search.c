#include <norm/query.h>
#include "memory_models.h"
#include "session_models.h"
#include "memory_search.h"
#include "records.h"

#include <nb/buf.h>
#include <nb/cleanup.h>
#include "errors.h"
#include <nb/utf8.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MEMORY_SEARCH_TERM_LIMIT 24
#define MEMORY_SEARCH_QUERY_MAX_BYTES 2048
#define MEMORY_SEARCH_RESULT_MAX_BYTES (64u * 1024u * 1024u)
#define MEMORY_SEARCH_ROW_MAX_BYTES (8u * 1024u * 1024u)

static void memory_terms_cleanup(nb_vec *terms)
{
	char **term;

	for (size_t _i_ = 0; _i_ < nb_vec_len(terms) && (term = (char **)nb_vec_at(terms, _i_));
	     _i_++)
		free(*term);

	nb_vec_cleanup(terms);
}

/* The model supplies useful query words or phrases. Whitespace separates
 * alternatives; no language-specific tokenizer or stopword dictionary is used. */
static int memory_terms_create(const char *query, nb_vec *terms)
{
	char *entry;
	size_t length = query ? strlen(query) : 0;
	size_t start = 0;
	int rc = (nb_vec_init(terms, sizeof(entry)), nb_vec_reserve(terms, 8) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;

	if (length > MEMORY_SEARCH_QUERY_MAX_BYTES)
		return NB_ERR_TOO_BIG;

	for (size_t position = 0; position <= length;) {
		unsigned cp = 0;
		size_t bytes = 1;

		if (position < length &&
		    !nb_utf8_read(query + position, length - position, &cp, &bytes))
			return NB_ERR_FORMAT;

		int separator = position == length || (cp < 128 && isspace((unsigned char)cp)) ||
				nb_utf8_classify_codepoint(cp) == NB_UTF8_CLASS_SEPARATOR;

		if (separator && position > start) {
			nb_buf folded NB_CLEANUP(nb_buf_cleanup) = {0};
			char *word NB_CLEANUP(nb_charp_cleanup) = NULL;

			rc = nb_utf8_casefold(nb_str_from_parts(query + start, position - start),
					       &folded, NULL);
			if (rc != NB_OK)
				return rc;

			word = orm_text_copy(nb_buf_str(&folded));
			if (!word)
				return NB_ERR_NOMEM;

			char **slot = nb_vec_push(terms);

			if (!slot)
				return NB_ERR_NOMEM;

			*slot = nb_charp_steal(&word);
			if (terms->len == MEMORY_SEARCH_TERM_LIMIT)
				break;
		}

		if (separator)
			start = position + bytes;

		position += bytes;
	}

	return 0;
}

static void memory_record_cleanup(struct memory_record *record)
{
	free(record->kind);
	free(record->key);
	free(record->value);
	free(record->source);
	free(record->details);
	free(record->session);
}

void memory_search_result_cleanup(struct memory_search_result *matches)
{
	struct memory_record *record;

	if (!matches)
		return;

	for (size_t _i_ = 0; _i_ < nb_vec_len(&matches->records) &&
			     (record = (struct memory_record *)nb_vec_at(&matches->records, _i_));
	     _i_++)
		memory_record_cleanup(record);

	nb_vec_cleanup(&matches->records);
}

static int memory_search_visible_sessions(struct db *db,
					  const struct memory_search_options *options,
					  nb_vec *visible)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	nb_vec_init(visible, sizeof(struct norm_value));
	if (!options->visible_fn)
		return 0;

	struct norm_value *sentinel = nb_vec_push(visible);
	if (!sentinel)
		return NB_ERR_NOMEM;

	*sentinel = norm_i64(-1);
	const size_t selected[] = {SESSION_ID, SESSION_DISPLAY_ID};
	const struct norm_select_query query = {
		.fields = selected, .field_count = 2, .max_rows = SIZE_MAX, .max_bytes = SIZE_MAX};
	int rc = orm_scope_begin(db, &lease);
	if (rc == 0)
		rc = orm_select_rows(&lease, &session_model, &query, &rows);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct session_row *row = norm_rows_at(rows, i);
		char *display NB_CLEANUP(nb_charp_cleanup) =
			orm_text_copy(row->display_id_missing ? NB_STR_LIT("") : row->display_id);

		if (!display)
			return NB_ERR_NOMEM;

		if (!options->visible_fn(display, options->user_id, options->visible_data))
			continue;

		struct norm_value *value = nb_vec_push(visible);
		if (!value)
			return NB_ERR_NOMEM;

		*value = norm_i64(row->id);
	}

	return rc;
}

struct search_record_row {
	int64_t id;
	int64_t session_id;
	int64_t updated_at;
	nb_str kind;
	nb_str key;
	nb_str value;
	nb_str source;
	nb_str details;
	bool details_missing;
	nb_str session;
	double importance;
	bool importance_missing;
	int score;
	int64_t memory_version;
};
static const struct norm_field search_record_fields[] = {
	NORM_FIELD(struct search_record_row, id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct search_record_row, session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct search_record_row, updated_at, NORM_TYPE_I64, 0),
	NORM_FIELD(struct search_record_row, kind, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct search_record_row, key, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct search_record_row, value, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct search_record_row, source, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct search_record_row, details, details_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct search_record_row, session, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct search_record_row, importance, importance_missing, NORM_TYPE_F64,
			    0),
	NORM_FIELD(struct search_record_row, score, NORM_TYPE_I32, 0),
	NORM_FIELD(struct search_record_row, memory_version, NORM_TYPE_I64, 0),
};
static const struct norm_model search_record_model =
	NORM_MODEL(struct search_record_row, "", search_record_fields);

enum search_column {
	SEARCH_KIND,
	SEARCH_ID,
	SEARCH_SESSION_ID,
	SEARCH_KEY,
	SEARCH_VALUE,
	SEARCH_SOURCE,
	SEARCH_DETAILS,
	SEARCH_UPDATED_AT,
	SEARCH_IMPORTANCE,
	SEARCH_SESSION,
	SEARCH_MEMORY_VERSION,
	SEARCH_REVISION,
	SEARCH_SCORE
};
struct search_query_row {
	nb_str kind;
	int64_t id;
	int64_t session_id;
	nb_str key;
	nb_str value;
	nb_str source;
	nb_str details;
	bool details_missing;
	int64_t updated_at;
	double importance;
	bool importance_missing;
	nb_str session;
	bool session_missing;
	int64_t memory_version;
	int64_t revision;
	int32_t score;
};
static const struct norm_field search_query_fields[] = {
	NORM_FIELD(struct search_query_row, kind, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct search_query_row, id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct search_query_row, session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct search_query_row, key, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct search_query_row, value, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct search_query_row, source, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct search_query_row, details, details_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct search_query_row, updated_at, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct search_query_row, importance, importance_missing, NORM_TYPE_F64,
			    0),
	NORM_NULLABLE_FIELD(struct search_query_row, session, session_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct search_query_row, memory_version, NORM_TYPE_I64, 0),
	NORM_FIELD(struct search_query_row, revision, NORM_TYPE_I64, 0),
	NORM_FIELD(struct search_query_row, score, NORM_TYPE_I32, 0),
};

static int memory_search_read(struct orm_scope *scope, int64_t session_id,
			      const struct memory_search_options *options, const nb_vec *visible,
			      struct norm_cursor **out)
{
	const struct norm_model raw_model = {.table = NB_STR_LIT("raw"),
					     .size = sizeof(struct search_query_row),
					     .alignment = _Alignof(struct search_query_row),
					     .fields = search_query_fields,
					     .field_count = 9};
	const struct norm_model scoped_model = {.table = NB_STR_LIT("scoped"),
						.size = sizeof(struct search_query_row),
						.alignment = _Alignof(struct search_query_row),
						.fields = search_query_fields,
						.field_count = 12};
	const struct norm_table_ref facts = norm_table(&fact_model, NB_STR_LIT("f"));
	const struct norm_table_ref candidates =
		norm_table(&memory_candidate_model, NB_STR_LIT("c"));
	const struct norm_table_ref episodes = norm_table(&memory_episode_model, NB_STR_LIT("e"));
	const struct norm_table_ref procedures =
		norm_table(&memory_procedure_model, NB_STR_LIT("p"));
	const struct norm_table_ref notes = norm_table(&memory_note_model, NB_STR_LIT("n"));
	const struct norm_table_ref summaries = norm_table(&memory_summary_model, NB_STR_LIT("m"));
	const struct norm_table_ref summary_scopes =
		norm_table(&memory_scope_model, NB_STR_LIT("scope"));
	const struct norm_table_ref private_notes =
		norm_table(&memory_note_model, NB_STR_LIT("private"));
	const struct norm_table_ref raw = norm_table(&raw_model, NB_STR_LIT("r"));
	const struct norm_table_ref sessions = norm_table(&session_model, NB_STR_LIT("s"));
	const struct norm_table_ref origin = norm_table(&memory_scope_model, NB_STR_LIT("origin"));
	const struct norm_table_ref current =
		norm_table(&memory_scope_model, NB_STR_LIT("current"));
	const struct norm_table_ref scoped = norm_table(&scoped_model, NB_STR_LIT("s"));
	const struct norm_term empty = norm_term_value(orm_string(""));
	const struct norm_term half = norm_term_value(norm_f64(0.5));
	const struct norm_term one = norm_term_value(norm_f64(1.0));
	const struct norm_term newline = norm_term_value(orm_string("\n"));
	const struct norm_value setting_keys[] = {orm_string("language"),
						  orm_string("preferred_language"),
						  orm_string("response_language"),
						  orm_string("language_preference"),
						  orm_string("preferred_response_language"),
						  orm_string("response_style"),
						  orm_string("style"),
						  orm_string("verbosity"),
						  orm_string("detail_level"),
						  orm_string("preferred_name"),
						  orm_string("user.preferred_name")};
	const struct norm_expr setting_prefix =
		norm_like(norm_table_col(&facts, FACT_KEY_NAME), NB_STR_LIT("response.%"));
	const struct norm_expr fact_conditions[] = {
		norm_eq(norm_table_col(&facts, FACT_IS_CURRENT), norm_i32(1)),
		norm_ne(norm_table_col(&facts, FACT_CATEGORY), orm_string("preference")),
		norm_not_in(norm_table_col(&facts, FACT_KEY_NAME), setting_keys,
			    NB_ARRAY_SIZE(setting_keys)),
		norm_not(&setting_prefix)};
	const struct norm_term fact_source[] = {
		norm_term_column(norm_table_col(&facts, FACT_SOURCE_TEXT)), empty};
	const struct norm_query_field fact_fields[] = {
		{.value = norm_term_value(orm_string("facts")), .into = SEARCH_KIND},
		{.value = norm_term_column(norm_table_col(&facts, FACT_ID)), .into = SEARCH_ID},
		{.value = norm_term_column(norm_table_col(&facts, FACT_SESSION_ID)),
		 .into = SEARCH_SESSION_ID},
		{.value = norm_term_column(norm_table_col(&facts, FACT_KEY_NAME)),
		 .into = SEARCH_KEY},
		{.value = norm_term_column(norm_table_col(&facts, FACT_VALUE_TEXT)),
		 .into = SEARCH_VALUE},
		{.value = norm_term_call(NORM_FUNCTION_COALESCE, fact_source, 2),
		 .into = SEARCH_SOURCE},
		{.value = norm_term_column(norm_table_col(&facts, FACT_CATEGORY)),
		 .into = SEARCH_DETAILS},
		{.value = norm_term_column(norm_table_col(&facts, FACT_UPDATED_AT)),
		 .into = SEARCH_UPDATED_AT},
		{.value = norm_term_column(norm_table_col(&facts, FACT_IMPORTANCE)),
		 .into = SEARCH_IMPORTANCE},
	};
	const struct norm_value setting_kinds[] = {orm_string("preference"), orm_string("rule"),
						   orm_string("legacy")};
	const struct norm_value tips[] = {orm_string("tip"), orm_string("lesson")};
	const struct norm_expr settings[] = {
		norm_in(norm_table_col(&candidates, CANDIDATE_KIND), setting_kinds, 3),
		norm_in(norm_table_col(&candidates, CANDIDATE_KEY), setting_keys,
			NB_ARRAY_SIZE(setting_keys)),
		norm_like(norm_table_col(&candidates, CANDIDATE_KEY), NB_STR_LIT("response.%"))};
	const struct norm_term unapplied = norm_term_value(orm_string("unapplied_settings"));
	const struct norm_term tip = norm_term_value(orm_string("tips"));
	const struct norm_term learned = norm_term_value(orm_string("learned_facts"));
	const struct norm_case candidate_kinds[] = {
		{.when = norm_or(settings, 3), .then = &unapplied},
		{.when = norm_in(norm_table_col(&candidates, CANDIDATE_KIND), tips, 2),
		 .then = &tip}};
	const struct norm_query_field candidate_fields[] = {
		{.value = norm_term_case(candidate_kinds, 2, &learned), .into = SEARCH_KIND},
		{.value = norm_term_column(norm_table_col(&candidates, CANDIDATE_ID)),
		 .into = SEARCH_ID},
		{.value = norm_term_column(norm_table_col(&candidates, CANDIDATE_SESSION_ID)),
		 .into = SEARCH_SESSION_ID},
		{.value = norm_term_column(norm_table_col(&candidates, CANDIDATE_KEY)),
		 .into = SEARCH_KEY},
		{.value = norm_term_column(norm_table_col(&candidates, CANDIDATE_VALUE)),
		 .into = SEARCH_VALUE},
		{.value = norm_term_column(norm_table_col(&candidates, CANDIDATE_SOURCE)),
		 .into = SEARCH_SOURCE},
		{.value = norm_term_column(norm_table_col(&candidates, CANDIDATE_KIND)),
		 .into = SEARCH_DETAILS},
		{.value = norm_term_column(norm_table_col(&candidates, CANDIDATE_CREATED_AT)),
		 .into = SEARCH_UPDATED_AT},
		{.value = half, .into = SEARCH_IMPORTANCE},
	};
	const size_t episode_detail_fields[] = {EPISODE_KEY_DECISIONS, EPISODE_ENTITIES,
						EPISODE_ARTIFACTS, EPISODE_TOOLS_USED,
						EPISODE_OUTCOME_TEXT};
	struct norm_term detail_defaults[5][2] = {0};
	struct norm_term detail_components[5] = {0};
	struct norm_term detail_joined[9] = {0};
	for (size_t i = 0; i < 5; i++) {
		detail_defaults[i][0] =
			norm_term_column(norm_table_col(&episodes, episode_detail_fields[i]));
		detail_defaults[i][1] = empty;
		detail_components[i] =
			norm_term_call(NORM_FUNCTION_COALESCE, detail_defaults[i], 2);
		if (!i)
			detail_joined[0] = detail_components[i];
		else {
			detail_joined[2 * i - 1] = norm_term_binary(
				NORM_BINARY_CONCAT, &detail_joined[2 * i - 2], &newline);
			detail_joined[2 * i] =
				norm_term_binary(NORM_BINARY_CONCAT, &detail_joined[2 * i - 1],
						 &detail_components[i]);
		}
	}

	const struct norm_term episode_task[] = {
		norm_term_column(norm_table_col(&episodes, EPISODE_TASK_TYPE)),
		norm_term_value(orm_string("task"))};
	const struct norm_query_field episode_fields[] = {
		{.value = norm_term_value(orm_string("past_tasks")), .into = SEARCH_KIND},
		{.value = norm_term_column(norm_table_col(&episodes, EPISODE_ID)),
		 .into = SEARCH_ID},
		{.value = norm_term_column(norm_table_col(&episodes, EPISODE_SESSION_ID)),
		 .into = SEARCH_SESSION_ID},
		{.value = norm_term_call(NORM_FUNCTION_COALESCE, episode_task, 2),
		 .into = SEARCH_KEY},
		{.value = norm_term_column(norm_table_col(&episodes, EPISODE_SUMMARY_TEXT)),
		 .into = SEARCH_VALUE},
		{.value = norm_term_column(norm_table_col(&episodes, EPISODE_SUMMARY_TEXT)),
		 .into = SEARCH_SOURCE},
		{.value = detail_joined[8], .into = SEARCH_DETAILS},
		{.value = norm_term_column(norm_table_col(&episodes, EPISODE_CREATED_AT)),
		 .into = SEARCH_UPDATED_AT},
		{.value = norm_term_column(norm_table_col(&episodes, EPISODE_IMPORTANCE)),
		 .into = SEARCH_IMPORTANCE},
	};
	const struct norm_term procedure_source[] = {
		norm_term_column(norm_table_col(&procedures, PROCEDURE_TRIGGER_TEXT)), empty};
	const struct norm_query_field procedure_fields[] = {
		{.value = norm_term_value(orm_string("unapplied_rules")), .into = SEARCH_KIND},
		{.value = norm_term_column(norm_table_col(&procedures, PROCEDURE_ID)),
		 .into = SEARCH_ID},
		{.value = norm_term_column(norm_table_col(&procedures, PROCEDURE_SESSION_ID)),
		 .into = SEARCH_SESSION_ID},
		{.value = norm_term_value(orm_string("legacy_rule")), .into = SEARCH_KEY},
		{.value = norm_term_column(norm_table_col(&procedures, PROCEDURE_RULE_TEXT)),
		 .into = SEARCH_VALUE},
		{.value = norm_term_call(NORM_FUNCTION_COALESCE, procedure_source, 2),
		 .into = SEARCH_SOURCE},
		{.value = norm_term_value(orm_string("Legacy rule; not an active preference")),
		 .into = SEARCH_DETAILS},
		{.value = norm_term_column(norm_table_col(&procedures, PROCEDURE_UPDATED_AT)),
		 .into = SEARCH_UPDATED_AT},
		{.value = half, .into = SEARCH_IMPORTANCE},
	};
	const struct norm_term note_prefix = norm_term_value(orm_string("Explicit user note: "));
	const struct norm_term note_scope = norm_term_column(norm_table_col(&notes, NOTE_SCOPE));
	const struct norm_query_field note_fields[] = {
		{.value = norm_term_value(orm_string("notes")), .into = SEARCH_KIND},
		{.value = norm_term_column(norm_table_col(&notes, NOTE_ID)), .into = SEARCH_ID},
		{.value = norm_term_column(norm_table_col(&notes, NOTE_SESSION_ID)),
		 .into = SEARCH_SESSION_ID},
		{.value = norm_term_column(norm_table_col(&notes, NOTE_TITLE)), .into = SEARCH_KEY},
		{.value = norm_term_column(norm_table_col(&notes, NOTE_TEXT)),
		 .into = SEARCH_VALUE},
		{.value = norm_term_column(norm_table_col(&notes, NOTE_SOURCE)),
		 .into = SEARCH_SOURCE},
		{.value = norm_term_binary(NORM_BINARY_CONCAT, &note_prefix, &note_scope),
		 .into = SEARCH_DETAILS},
		{.value = norm_term_column(norm_table_col(&notes, NOTE_UPDATED_AT)),
		 .into = SEARCH_UPDATED_AT},
		{.value = one, .into = SEARCH_IMPORTANCE},
	};
	const struct norm_expr note_visible[] = {
		norm_eq(norm_table_col(&notes, NOTE_SCOPE), orm_string("project")),
		norm_eq(norm_table_col(&notes, NOTE_SESSION_ID), norm_i64(session_id))};
	const struct norm_query_field any_fields[] = {
		{.value = norm_term_value(norm_i32(1)), .into = 0}};
	const struct norm_expr private_conditions[] = {
		norm_columns_eq(norm_table_col(&private_notes, NOTE_SESSION_ID),
				norm_table_col(&summaries, SUMMARY_SESSION_ID)),
		norm_eq(norm_table_col(&private_notes, NOTE_SCOPE), orm_string("session"))};
	const struct norm_query any_private = {.from = &private_notes,
					       .projection = &orm_integer_model,
					       .fields = any_fields,
					       .field_count = 1,
					       .where = norm_and(private_conditions, 2)};
	const struct norm_expr private_exists = norm_exists(&any_private);
	const struct norm_expr summary_visible[] = {
		norm_eq(norm_table_col(&summaries, SUMMARY_SESSION_ID), norm_i64(session_id)),
		norm_not(&private_exists)};
	const struct norm_expr summary_conditions[] = {
		norm_ne(norm_table_col(&summaries, SUMMARY_TEXT), orm_string("")),
		norm_columns_eq(norm_table_col(&summaries, SUMMARY_MEMORY_VERSION),
				norm_table_col(&summary_scopes, SCOPE_GENERATION)),
		norm_columns_eq(norm_table_col(&summaries, SUMMARY_CONTENT_REVISION),
				norm_table_col(&summary_scopes, SCOPE_CONTENT_REVISION)),
		norm_or(summary_visible, 2)};
	const struct norm_join summary_join = {
		.table = &summary_scopes,
		.kind = NORM_JOIN_INNER,
		.on = norm_columns_eq(norm_table_col(&summary_scopes, SCOPE_SESSION_ID),
				      norm_table_col(&summaries, SUMMARY_SESSION_ID))};
	const struct norm_query_field summary_fields[] = {
		{.value = norm_term_value(orm_string("summary")), .into = SEARCH_KIND},
		{.value = norm_term_column(norm_table_col(&summaries, SUMMARY_SESSION_ID)),
		 .into = SEARCH_ID},
		{.value = norm_term_column(norm_table_col(&summaries, SUMMARY_SESSION_ID)),
		 .into = SEARCH_SESSION_ID},
		{.value = norm_term_column(norm_table_col(&summaries, SUMMARY_TITLE)),
		 .into = SEARCH_KEY},
		{.value = norm_term_column(norm_table_col(&summaries, SUMMARY_TEXT)),
		 .into = SEARCH_VALUE},
		{.value = norm_term_value(orm_string("Model-organized historical context")),
		 .into = SEARCH_SOURCE},
		{.value = norm_term_column(norm_table_col(&summaries, SUMMARY_SOURCES)),
		 .into = SEARCH_DETAILS},
		{.value = norm_term_column(norm_table_col(&summaries, SUMMARY_UPDATED_AT)),
		 .into = SEARCH_UPDATED_AT},
		{.value = half, .into = SEARCH_IMPORTANCE},
	};
	const struct norm_query branches[] = {
		{.from = &candidates,
		 .projection = &raw_model,
		 .fields = candidate_fields,
		 .field_count = 9},
		{.from = &episodes,
		 .projection = &raw_model,
		 .fields = episode_fields,
		 .field_count = 9},
		{.from = &procedures,
		 .projection = &raw_model,
		 .fields = procedure_fields,
		 .field_count = 9},
		{.from = &notes,
		 .projection = &raw_model,
		 .fields = note_fields,
		 .field_count = 9,
		 .where = options->all == 1 ? (struct norm_expr){0} : norm_or(note_visible, 2)},
		{.from = &summaries,
		 .joins = &summary_join,
		 .join_count = 1,
		 .projection = &raw_model,
		 .fields = summary_fields,
		 .field_count = 9,
		 .where = norm_and(summary_conditions, options->all == 1 ? 3 : 4)},
	};
	struct norm_query_set unions[5] = {0};
	for (size_t i = 0; i < 5; i++)
		unions[i] = (struct norm_query_set){.operation = NORM_SET_UNION_ALL,
						    .query = &branches[i]};

	const struct norm_query raw_query = {.from = &facts,
					     .projection = &raw_model,
					     .fields = fact_fields,
					     .field_count = 9,
					     .where = norm_and(fact_conditions, 4),
					     .sets = unions,
					     .set_count = 5};
	const struct norm_join scoped_joins[] = {
		{.table = &sessions,
		 .kind = NORM_JOIN_INNER,
		 .on = norm_columns_eq(norm_table_col(&sessions, SESSION_ID),
				       norm_table_col(&raw, SEARCH_SESSION_ID))},
		{.table = &origin,
		 .kind = NORM_JOIN_LEFT,
		 .on = norm_columns_eq(norm_table_col(&origin, SCOPE_SESSION_ID),
				       norm_table_col(&raw, SEARCH_SESSION_ID))},
		{.table = &current,
		 .kind = NORM_JOIN_LEFT,
		 .on = norm_eq(norm_table_col(&current, SCOPE_SESSION_ID), norm_i64(session_id))},
	};
	const struct norm_term display_defaults[] = {
		norm_term_column(norm_table_col(&sessions, SESSION_DISPLAY_ID)), empty};
	const struct norm_term session_defaults[] = {
		norm_term_call(NORM_FUNCTION_NULLIF, display_defaults, 2),
		norm_term_column(norm_table_col(&sessions, SESSION_NAME))};
	const struct norm_term memory_defaults[] = {
		norm_term_column(norm_table_col(&origin, SCOPE_GENERATION)),
		norm_term_value(norm_i64(0))};
	const struct norm_value by_value[] = {
		orm_string("past_tasks"), orm_string("unapplied_settings"),
		orm_string("unapplied_rules"), orm_string("notes"), orm_string("summary")};
	const struct norm_term raw_value = norm_term_column(norm_table_col(&raw, SEARCH_VALUE));
	const struct norm_term raw_key = norm_term_column(norm_table_col(&raw, SEARCH_KEY));
	const struct norm_case revision_key[] = {
		{.when = norm_in(norm_table_col(&raw, SEARCH_KIND), by_value, 5),
		 .then = &raw_value}};
	const struct norm_term revision_partition[] = {
		norm_term_column(norm_table_col(&raw, SEARCH_KIND)),
		norm_term_case(revision_key, 1, &raw_key)};
	const struct norm_query_order revision_order[] = {
		{.value = norm_term_column(norm_table_col(&raw, SEARCH_UPDATED_AT)),
		 .direction = NORM_DESC},
		{.value = norm_term_column(norm_table_col(&raw, SEARCH_ID)),
		 .direction = NORM_DESC}};
	struct norm_query_field scoped_fields[12] = {0};
	for (size_t i = 0; i < 9; i++)
		scoped_fields[i] = (struct norm_query_field){
			.value = norm_term_column(norm_table_col(&raw, i)), .into = i};

	scoped_fields[9] = (struct norm_query_field){
		.value = norm_term_call(NORM_FUNCTION_COALESCE, session_defaults, 2), .into = 9};
	scoped_fields[10] = (struct norm_query_field){
		.value = norm_term_call(NORM_FUNCTION_COALESCE, memory_defaults, 2), .into = 10};
	scoped_fields[11] = (struct norm_query_field){
		.value = norm_term_row_number((struct norm_window){.partition = revision_partition,
								   .partition_count = 2,
								   .order = revision_order,
								   .order_count = 2}),
		.into = 11};
	const struct norm_expr cross_project[] = {
		norm_columns_eq(norm_table_col(&origin, SCOPE_OWNER),
				norm_table_col(&current, SCOPE_OWNER)),
		norm_ne(norm_table_col(&current, SCOPE_PROJECT), orm_string("")),
		norm_columns_eq(norm_table_col(&origin, SCOPE_PROJECT),
				norm_table_col(&current, SCOPE_PROJECT))};
	const struct norm_expr scope_visible[] = {
		norm_eq(norm_table_col(&raw, SEARCH_SESSION_ID), norm_i64(session_id)),
		norm_and(cross_project, options->all == 1 ? 1 : 3)};
	const struct norm_value learned_kinds[] = {orm_string("learned_facts"), orm_string("tips"),
						   orm_string("notes"),
						   orm_string("unapplied_settings")};
	const char *kind = options->kind ? options->kind : "";
	const struct norm_expr kind_conditions[] = {
		norm_eq(norm_table_col(&raw, SEARCH_KIND), orm_string(kind)),
		norm_in(norm_table_col(&raw, SEARCH_KIND), learned_kinds, 4)};
	struct norm_expr scoped_conditions[4] = {0};
	size_t condition_count = 0;
	scoped_conditions[condition_count++] =
		norm_or(scope_visible, options->all == 1 || options->project == 1 ? 2 : 1);
	if (options->record_id)
		scoped_conditions[condition_count++] =
			norm_eq(norm_table_col(&raw, SEARCH_ID), norm_i64(options->record_id));

	if (*kind)
		scoped_conditions[condition_count++] =
			norm_or(kind_conditions, !strcmp(kind, "learned_notes") ? 2 : 1);

	if (options->visible_fn)
		scoped_conditions[condition_count++] =
			norm_in(norm_table_col(&raw, SEARCH_SESSION_ID),
				(const struct norm_value *)visible->data, visible->len);

	const struct norm_query scoped_query = {
		.from = &raw,
		.joins = scoped_joins,
		.join_count = 3,
		.projection = &scoped_model,
		.fields = scoped_fields,
		.field_count = 12,
		.where = norm_and(scoped_conditions, condition_count)};
	const struct norm_cte ctes[] = {{.model = &raw_model, .query = &raw_query},
					{.model = &scoped_model, .query = &scoped_query}};
	const size_t selected[] = {SEARCH_ID, SEARCH_SESSION_ID, SEARCH_UPDATED_AT,
				   SEARCH_KIND, SEARCH_KEY, SEARCH_VALUE, SEARCH_SOURCE,
				   SEARCH_DETAILS, SEARCH_SESSION, SEARCH_IMPORTANCE,
				   SEARCH_KIND, SEARCH_MEMORY_VERSION};
	struct norm_query_field fields[12] = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(fields); i++)
		fields[i] = (struct norm_query_field){
			.value = norm_term_column(norm_table_col(&scoped, selected[i])), .into = i};

	const struct norm_term session_final[] = {
		norm_term_column(norm_table_col(&scoped, SEARCH_SESSION)), empty};
	fields[8].value = norm_term_call(NORM_FUNCTION_COALESCE, session_final, 2);
	fields[10].value = norm_term_value(norm_i32(0));
	const struct norm_query_order order[] = {
		{.value = norm_term_column(norm_table_col(&scoped, SEARCH_UPDATED_AT)),
		 .direction = NORM_DESC},
		{.value = norm_term_column(norm_table_col(&scoped, SEARCH_IMPORTANCE)),
		 .direction = NORM_DESC,
		 .nulls = NORM_NULLS_LAST},
		{.value = norm_term_column(norm_table_col(&scoped, SEARCH_ID)),
		 .direction = NORM_DESC}};
	const struct norm_result_options limits = {
		.max_rows = SIZE_MAX, .max_row_bytes = MEMORY_SEARCH_ROW_MAX_BYTES};
	const struct norm_query query = {
		.with = ctes,
		.with_count = NB_ARRAY_SIZE(ctes),
		.from = &scoped,
		.projection = &search_record_model,
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.where = norm_eq(norm_table_col(&scoped, SEARCH_REVISION), norm_i64(1)),
		.order = order,
		.order_count = NB_ARRAY_SIZE(order),
		.result_options = &limits};
	struct norm_error error = {0};

	return orm_status(scope->database,
			  norm_query_cursor_open(scope->connection, &query, orm_scope_call(scope),
						 out, &error),
			  &error);
}

struct memory_search_match {
	struct memory_record record;
	bool importance_missing;
	size_t bytes;
};

static void memory_search_match_cleanup(struct memory_search_match *match)
{
	memory_record_cleanup(&match->record);
	memset(match, 0, sizeof(*match));
}

static struct memory_search_match memory_search_match_steal(struct memory_search_match *owner)
{
	struct memory_search_match value = *owner;

	memset(owner, 0, sizeof(*owner));
	return value;
}

static void memory_search_matches_cleanup(nb_vec *matches)
{
	for (size_t i = 0; i < matches->len; i++)
		memory_search_match_cleanup(nb_vec_at(matches, i));

	nb_vec_destroy(matches);
}

static void memory_search_cursor_cleanup(struct norm_cursor **owner)
{
	if (*owner)
		norm_cursor_close(owner, NULL);
}

static int memory_search_compare(const void *left, const void *right)
{
	const struct memory_search_match *a = left;
	const struct memory_search_match *b = right;

	if (a->record.score != b->record.score)
		return a->record.score > b->record.score ? -1 : 1;

	if (a->record.updated_at != b->record.updated_at)
		return a->record.updated_at > b->record.updated_at ? -1 : 1;

	if (a->importance_missing != b->importance_missing)
		return a->importance_missing ? 1 : -1;

	if (a->record.importance != b->record.importance)
		return a->record.importance > b->record.importance ? -1 : 1;

	if (a->record.id != b->record.id)
		return a->record.id > b->record.id ? -1 : 1;

	return 0;
}

static void memory_search_match_swap(struct memory_search_match *left,
				     struct memory_search_match *right)
{
	struct memory_search_match value = memory_search_match_steal(left);

	*left = memory_search_match_steal(right);
	*right = memory_search_match_steal(&value);
}

/* The root is the worst retained match, so a bounded result never needs to
 * retain every candidate. Every visible candidate is still scored. */
static void memory_search_heap_up(nb_vec *matches)
{
	size_t child = matches->len - 1;

	while (child) {
		size_t parent = (child - 1) / 2;
		struct memory_search_match *a = nb_vec_at(matches, parent);
		struct memory_search_match *b = nb_vec_at(matches, child);

		if (memory_search_compare(a, b) >= 0)
			return;

		memory_search_match_swap(a, b);
		child = parent;
	}
}

static void memory_search_heap_down(nb_vec *matches)
{
	size_t parent = 0;

	while (parent < matches->len / 2) {
		size_t child = parent * 2 + 1;

		if (child + 1 < matches->len &&
		    memory_search_compare(nb_vec_at(matches, child),
					  nb_vec_at(matches, child + 1)) < 0)
			child++;

		struct memory_search_match *a = nb_vec_at(matches, parent);
		struct memory_search_match *b = nb_vec_at(matches, child);

		if (memory_search_compare(a, b) >= 0)
			return;

		memory_search_match_swap(a, b);
		parent = child;
	}
}

static int memory_search_score(const struct search_record_row *row, const nb_vec *terms,
			       int *out)
{
	const nb_str values[] = {row->key, row->value,
				 row->details_missing ? NB_STR_NULL : row->details, row->source};
	const int weights[] = {4, 2, 1, 1};
	int score = 0;

	for (size_t field = 0; field < NB_ARRAY_SIZE(values) && terms->len; field++) {
		if (!values[field].len)
			continue;

		nb_buf folded NB_CLEANUP(nb_buf_cleanup) = {0};
		int rc = nb_utf8_casefold(values[field], &folded, NULL);

		if (rc != NB_OK)
			return rc;

		for (size_t i = 0; i < terms->len; i++) {
			const char *const *term = nb_vec_at(terms, i);

			if (strstr(nb_buf_cstr(&folded), *term))
				score += weights[field];
		}
	}

	*out = score;
	return NB_OK;
}

static int memory_search_row_bytes(const struct search_record_row *row, size_t *out)
{
	const nb_str values[] = {row->kind, row->key, row->value, row->source,
				 row->details_missing ? NB_STR_NULL : row->details, row->session};
	/* Account for both growing vectors and the final result vector. */
	size_t bytes = 2 * (sizeof(struct memory_search_match) + sizeof(struct memory_record));

	for (size_t i = 0; i < NB_ARRAY_SIZE(values); i++) {
		if (values[i].len >= MEMORY_SEARCH_RESULT_MAX_BYTES - bytes)
			return NB_ERR_TOO_BIG;

		bytes += values[i].len + 1;
	}

	*out = bytes;
	return NB_OK;
}

static int memory_search_copy_record(const struct search_record_row *row,
				     struct memory_record *record)
{
	record->id = row->id;
	record->session_id = row->session_id;
	record->updated_at = row->updated_at;
	record->kind = orm_text_copy(row->kind);
	record->key = orm_text_copy(row->key);
	record->value = orm_text_copy(row->value);
	record->source = orm_text_copy(row->source);
	record->details = row->details_missing ? strdup("") : orm_text_copy(row->details);
	record->session = orm_text_copy(row->session);
	record->importance = row->importance;
	record->memory_version = row->memory_version;
	if (!record->kind || !record->key || !record->value || !record->source ||
	    !record->details || !record->session)
		return NB_ERR_NOMEM;

	return NB_OK;
}

static int memory_search_collect(struct norm_cursor *cursor, const struct db *db,
				 const struct memory_search_options *options, const nb_vec *terms,
				 nb_vec *matches)
{
	struct norm_error error = {0};
	size_t retained = 0;
	bool present = false;
	int rc = NB_OK;

	nb_vec_init(matches, sizeof(struct memory_search_match));
	while ((rc = orm_status(db, norm_cursor_next(cursor, &present, &error), &error)) == NB_OK &&
	       present) {
		const struct search_record_row *row = norm_cursor_row(cursor);
		struct memory_search_match match NB_CLEANUP(memory_search_match_cleanup) = {
			.record = {.id = row->id,
				   .updated_at = row->updated_at,
				   .importance = row->importance},
			.importance_missing = row->importance_missing};

		rc = memory_search_score(row, terms, &match.record.score);
		if (rc != NB_OK)
			return rc;

		if (terms->len && !options->include_unmatched && !match.record.score)
			continue;

		bool replace = options->limit > 0 && matches->len >= (size_t)options->limit;
		struct memory_search_match *worst = replace ? nb_vec_at(matches, 0) : NULL;

		if (worst && memory_search_compare(&match, worst) >= 0)
			continue;

		rc = memory_search_row_bytes(row, &match.bytes);
		if (rc != NB_OK)
			return rc;

		size_t released = worst ? worst->bytes : 0;

		if (match.bytes > MEMORY_SEARCH_RESULT_MAX_BYTES - (retained - released))
			return NB_ERR_TOO_BIG;

		rc = memory_search_copy_record(row, &match.record);
		if (rc != NB_OK)
			return rc;

		retained = retained - released + match.bytes;
		if (worst) {
			memory_search_match_cleanup(worst);
			*worst = memory_search_match_steal(&match);
			memory_search_heap_down(matches);
		} else {
			struct memory_search_match *slot = nb_vec_push(matches);

			if (!slot)
				return NB_ERR_NOMEM;

			*slot = memory_search_match_steal(&match);
			if (options->limit > 0)
				memory_search_heap_up(matches);
		}
	}

	return rc;
}

int memory_search_records(struct db *db, int64_t session_id,
			  const struct memory_search_options *options,
			  struct memory_search_result *out)
{
	struct memory_search_result result NB_CLEANUP(memory_search_result_cleanup) = {0};
	nb_vec terms NB_CLEANUP(memory_terms_cleanup) = {0};
	nb_vec visibility NB_CLEANUP(nb_vec_cleanup) = {0};
	nb_vec matches NB_CLEANUP(memory_search_matches_cleanup) = {0};
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_cursor *cursor NB_CLEANUP(memory_search_cursor_cleanup) = NULL;
	int rc;

	if (!db_is_open(db) || !options || !out || session_id <= 0)
		return NB_ERR_INVAL;

	if (out->records.data)
		return NB_ERR_EXISTS;

	rc = memory_terms_create(options->query, &terms);
	if (rc == NB_OK)
		rc = memory_search_visible_sessions(db, options, &visibility);

	nb_vec_init(&result.records, sizeof(struct memory_record));
	if (rc != NB_OK)
		return rc;

	if (options->query && *options->query && !terms.len && !options->include_unmatched) {
		nb_vec_move(&out->records, &result.records);
		return NB_OK;
	}

	rc = orm_scope_begin(db, &lease);
	if (rc == NB_OK)
		rc = memory_search_read(&lease, session_id, options, &visibility, &cursor);

	if (rc == NB_OK)
		rc = memory_search_collect(cursor, db, options, &terms, &matches);

	if (cursor) {
		struct norm_error error = {0};
		int closed = orm_status(db, norm_cursor_close(&cursor, &error), &error);

		if (rc == NB_OK)
			rc = closed;
	}

	if (rc != NB_OK)
		return rc;

	if (matches.len > 1)
		qsort(matches.data, matches.len, sizeof(struct memory_search_match),
		      memory_search_compare);

	if (!nb_vec_reserve(&result.records, matches.len))
		return NB_ERR_NOMEM;

	for (size_t i = 0; i < matches.len; i++) {
		struct memory_search_match *match = nb_vec_at(&matches, i);
		struct memory_record *record = nb_vec_push(&result.records);

		if (!record)
			return NB_ERR_NOMEM;

		*record = match->record;
		memset(&match->record, 0, sizeof(match->record));
	}

	nb_vec_move(&out->records, &result.records);
	return NB_OK;
}
