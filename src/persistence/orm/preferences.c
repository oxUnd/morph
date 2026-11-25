#include <norm/query.h>
#include "memory_models.h"
#include "history_models.h"
#include "preferences.h"
#include "records.h"
#include "errors.h"
#include <nb/buf.h>
#include <nb/random.h>
#include <nb/encoding.h>
#include <nb/utf8.h>

struct preference_row {
	nb_str key;
	nb_str value;
	bool value_missing;
	nb_str scope;
	int64_t id;
	nb_str source;
	bool source_missing;
};
static const struct norm_field preference_fields[] = {
	NORM_FIELD(struct preference_row, key, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct preference_row, value, value_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct preference_row, scope, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct preference_row, id, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct preference_row, source, source_missing, NORM_TYPE_TEXT, 0),
};
static const struct norm_model preference_model =
	NORM_MODEL(struct preference_row, "memory_preferences", preference_fields);

const char *preference_key_borrow(const char *key)
{
	if (!key)
		return NULL;

	if (!strcmp(key, "preferred_language") || !strcmp(key, "language") ||
	    !strcmp(key, "response_language") || !strcmp(key, "language_preference") ||
	    !strcmp(key, "preferred_response_language"))
		return "response.language";

	if (!strcmp(key, "response_style") || !strcmp(key, "style") || !strcmp(key, "verbosity") ||
	    !strcmp(key, "detail_level"))
		return "response.detail";

	if (!strcmp(key, "preferred_name"))
		return "user.preferred_name";

	return key;
}

int preference_bind(struct db *db, int64_t session_id, const char *owner, const char *project)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_write_result result = {0};

	if (session_id <= 0 || !owner || !*owner)
		return NB_ERR_INVAL;

	const struct memory_scope_row row = {.session_id = session_id,
					     .owner = nb_str_from_cstr(owner),
					     .project = nb_str_from_cstr(project ? project : "")};
	const struct norm_term project_value =
		norm_term_excluded(norm_col(&memory_scope_model, SCOPE_PROJECT));
	const struct norm_term current_owner =
		norm_term_column(norm_col(&memory_scope_model, SCOPE_OWNER));
	const struct norm_term incoming_owner =
		norm_term_excluded(norm_col(&memory_scope_model, SCOPE_OWNER));
	const struct norm_term current_project =
		norm_term_column(norm_col(&memory_scope_model, SCOPE_PROJECT));
	const struct norm_expr compatible[] = {
		norm_eq(norm_col(&memory_scope_model, SCOPE_PROJECT), orm_string("")),
		norm_compare_terms(&current_project, NORM_COMPARE_EQ, &project_value),
	};
	const struct norm_expr allowed[] = {
		norm_compare_terms(&current_owner, NORM_COMPARE_EQ, &incoming_owner),
		norm_or(compatible, 2)};
	const struct norm_assignment set[] = {
		{.column = norm_col(&memory_scope_model, SCOPE_PROJECT),
		 .expression = &project_value}};
	const size_t keys[] = {SCOPE_SESSION_ID};
	const size_t fields[] = {SCOPE_SESSION_ID, SCOPE_OWNER, SCOPE_PROJECT};
	const struct norm_conflict conflict = {.action = NORM_CONFLICT_UPDATE,
					       .fields = keys,
					       .field_count = 1,
					       .set = set,
					       .set_count = 1,
					       .where = norm_and(allowed, 2)};
	const struct norm_insert_options options = {
		.fields = fields, .field_count = 3, .conflict = &conflict};
	struct norm_error error = {0};
	int rc = orm_scope_begin(db, &lease);

	if (rc == 0)
		rc = orm_status(db,
				norm_insert(lease.connection, &memory_scope_model, &row, &options,
					    NULL, &result, &error),
				&error);

	return rc ? rc : result.affected_rows ? 0 : NB_ERR_NOT_PERMITTED;
}

int preference_rebind_project(struct db *db, int64_t session_id, const char *expected_project,
			      const char *project)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_write_result result = {0};

	if (session_id <= 0 || !expected_project || !project || !project[0])
		return NB_ERR_INVAL;

	int rc = orm_scope_begin(db, &lease);
	struct norm_error error = {0};
	const struct norm_select_query select = {
		.where = norm_eq(norm_col(&memory_scope_model, SCOPE_SESSION_ID),
				 norm_i64(session_id))};

	if (rc == 0)
		rc = orm_status(db,
				norm_select(lease.connection, &memory_scope_model, &select, NULL,
					    &rows, &error),
				&error);

	if (rc != 0 || !norm_rows_count(rows))
		return rc;

	const struct memory_scope_row *row = norm_rows_at(rows, 0);
	if (nb_str_eq_cstr(row->project, project))
		return 0;

	if (row->project.len && !nb_str_eq_cstr(row->project, expected_project))
		return NB_ERR_STALE;

	const struct norm_assignment set[] = {
		{.column = norm_col(&memory_scope_model, SCOPE_PROJECT),
		 .value = orm_string(project)}};
	const struct norm_expr previous[] = {
		norm_eq(norm_col(&memory_scope_model, SCOPE_PROJECT), orm_string(expected_project)),
		norm_eq(norm_col(&memory_scope_model, SCOPE_PROJECT), orm_string("")),
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&memory_scope_model, SCOPE_SESSION_ID), norm_i64(session_id)),
		norm_or(previous, 2),
	};
	const struct norm_update_query update = {
		.set = set, .set_count = 1, .where = norm_and(conditions, 2)};
	rc = orm_status(
		db,
		norm_update(lease.connection, &memory_scope_model, &update, NULL, &result, &error),
		&error);
	return rc ? rc : result.affected_rows ? 0 : NB_ERR_STALE;
}

static int preference_valid_key(const char *key)
{
	return key && strlen(key) <= 128 && nb_str_is_alnum(nb_str_from_cstr(key), "._-");
}

struct preference_insert_row {
	nb_str owner;
	nb_str scope;
	nb_str target;
	nb_str key;
	nb_str value;
	bool value_missing;
	nb_str source;
	int64_t session_id;
	nb_str event_token;
	int64_t source_order;
};
static const struct norm_field preference_insert_fields[] = {
	NORM_FIELD(struct preference_insert_row, owner, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct preference_insert_row, scope, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct preference_insert_row, target, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct preference_insert_row, key, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct preference_insert_row, value, value_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct preference_insert_row, source, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct preference_insert_row, session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct preference_insert_row, event_token, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct preference_insert_row, source_order, NORM_TYPE_I64, 0),
};
static const struct norm_model preference_insert_model =
	NORM_MODEL(struct preference_insert_row, "memory_preferences", preference_insert_fields);
static const size_t preference_insert_targets[] = {
	PREFERENCE_OWNER,      PREFERENCE_SCOPE,       PREFERENCE_TARGET,
	PREFERENCE_KEY,	       PREFERENCE_VALUE,       PREFERENCE_SOURCE,
	PREFERENCE_SESSION_ID, PREFERENCE_EVENT_TOKEN, PREFERENCE_SOURCE_ORDER};

struct preference_order_query {
	struct norm_table_ref histories[2];
	struct norm_term id;
	struct norm_term prefix;
	struct norm_term event;
	struct norm_term token;
	struct norm_expr match[3];
	struct norm_query_field fields[2];
	struct norm_term maximum;
	struct norm_term defaults[2];
	struct norm_term coalesced;
	struct norm_term one;
	struct norm_query queries[2];
	struct norm_term alternatives[2];
	struct norm_term value;
};

static void preference_order_query_init(struct preference_order_query *out, int64_t session_id,
					const char *token)
{
	out->histories[0] = norm_table(&history_model, NB_STR_LIT("h"));
	out->histories[1] = norm_table(&history_model, NB_STR_LIT("n"));
	out->id = norm_term_column(norm_table_col(&out->histories[0], HIST_ID));
	out->prefix = norm_term_value(orm_string("history:"));
	out->event = norm_term_binary(NORM_BINARY_CONCAT, &out->prefix, &out->id);
	out->token = norm_term_value(orm_string(token));
	out->match[0] =
		norm_eq(norm_table_col(&out->histories[0], HIST_SESSION_ID), norm_i64(session_id));
	out->match[1] = norm_eq(norm_table_col(&out->histories[0], HIST_ROLE), orm_string("user"));
	out->match[2] = norm_compare_terms(&out->token, NORM_COMPARE_EQ, &out->event);
	out->fields[0] = (struct norm_query_field){.value = out->id, .into = 0};
	out->queries[0] = (struct norm_query){.from = &out->histories[0],
					      .projection = &orm_integer_model,
					      .fields = &out->fields[0],
					      .field_count = 1,
					      .where = norm_and(out->match, 3)};
	out->maximum = norm_term_column(norm_table_col(&out->histories[1], HIST_ID));
	out->defaults[0] = norm_term_call(NORM_FUNCTION_MAX, &out->maximum, 1);
	out->defaults[1] = norm_term_value(norm_i64(0));
	out->coalesced = norm_term_call(NORM_FUNCTION_COALESCE, out->defaults, 2);
	out->one = norm_term_value(norm_i64(1));
	out->fields[1] = (struct norm_query_field){
		.value = norm_term_binary(NORM_BINARY_ADD, &out->coalesced, &out->one), .into = 0};
	out->queries[1] = (struct norm_query){.from = &out->histories[1],
					      .projection = &orm_integer_model,
					      .fields = &out->fields[1],
					      .field_count = 1};
	out->alternatives[0] = norm_term_scalar(&out->queries[0]);
	out->alternatives[1] = norm_term_scalar(&out->queries[1]);
	out->value = norm_term_call(NORM_FUNCTION_COALESCE, out->alternatives, 2);
}

int preference_set(struct db *db, int64_t session_id, const char *scope, const char *key,
		   const char *value, const char *source, const char *event_token)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;

	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	char token[64];
	int rc;

	key = preference_key_borrow(key);
	if (!db_is_open(db) || !preference_valid_key(key) || !scope ||
	    (strcmp(scope, "personal") && strcmp(scope, "project") && strcmp(scope, "session")) ||
	    (value &&
	     (!*value || strlen(value) > 1024 || nb_utf8_validate(nb_str_from_cstr(value), NULL))))
		return NB_ERR_INVAL;

	if (value && !strcmp(key, "response.language")) {
		if (!strcmp(value, "zh") || !strcmp(value, "中文"))
			value = "Chinese";
		else if (!strcmp(value, "en") || !strcmp(value, "英文"))
			value = "English";
	}

	if (!event_token || !*event_token) {
		rc = ((file_status_1 = ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
					: !(nb_buf_clear(&file_value_1),
					    nb_buf_append_cstr(&file_value_1, "pref_"))
						? NB_ERR_NOMEM
						: nb_random_hex(&file_value_1, 16, NULL))) != NB_OK
			      ? file_status_1
		      : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), token,
							sizeof(token))) == NB_ERR_RANGE
			      ? NB_ERR_NO_SPACE
			      : file_status_1);
		if (rc != 0)
			return rc;

		event_token = token;
	}

	rc = orm_scope_begin(db, &lease);
	struct norm_error error = {0};
	const struct norm_expr present[] = {
		norm_eq(norm_col(&memory_scope_model, SCOPE_SESSION_ID), norm_i64(session_id)),
		strcmp(scope, "project")
			? (struct norm_expr){0}
			: norm_ne(norm_col(&memory_scope_model, SCOPE_PROJECT), orm_string("")),
	};
	const struct norm_select_query exists = {
		.where = norm_and(present, strcmp(scope, "project") ? 1 : 2),
		.has_limit = true,
		.limit = 1};

	if (rc == 0)
		rc = orm_status(db,
				norm_select(lease.connection, &memory_scope_model, &exists, NULL,
					    &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return NB_ERR_NOT_FOUND;

	orm_rows_cleanup(&rows);
	const struct norm_table_ref scopes = norm_table(&memory_scope_model, NB_STR_LIT("s"));
	const struct norm_term session =
		norm_term_column(norm_table_col(&scopes, SCOPE_SESSION_ID));
	const struct norm_term target =
		!strcmp(scope, "personal") ? norm_term_value(orm_string(""))
		: !strcmp(scope, "project")
			? norm_term_column(norm_table_col(&scopes, SCOPE_PROJECT))
			: norm_term_cast(&session, NORM_TYPE_TEXT);
	struct preference_order_query order = {0};
	preference_order_query_init(&order, session_id, event_token);
	const struct norm_query_field fields[] = {
		{.value = norm_term_column(norm_table_col(&scopes, SCOPE_OWNER)), .into = 0},
		{.value = norm_term_value(orm_string(scope)), .into = 1},
		{.value = target, .into = 2},
		{.value = norm_term_value(orm_string(key)), .into = 3},
		{.value = norm_term_value(orm_string(value)), .into = 4},
		{.value = norm_term_value(orm_string(source ? source : "")), .into = 5},
		{.value = session, .into = 6},
		{.value = norm_term_value(orm_string(event_token)), .into = 7},
		{.value = order.value, .into = 8},
	};
	const struct norm_expr scope_conditions[] = {
		norm_eq(norm_table_col(&scopes, SCOPE_SESSION_ID), norm_i64(session_id)),
		strcmp(scope, "project")
			? (struct norm_expr){0}
			: norm_ne(norm_table_col(&scopes, SCOPE_PROJECT), orm_string("")),
	};
	const struct norm_query insert = {
		.from = &scopes,
		.projection = &preference_insert_model,
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.where = norm_and(scope_conditions, strcmp(scope, "project") ? 1 : 2)};
	const struct norm_conflict ignore = {.action = NORM_CONFLICT_NOTHING};
	struct norm_write_result result = {0};

	rc = orm_status(db,
			norm_insert_select(lease.connection, &memory_preference_model,
					   preference_insert_targets,
					   NB_ARRAY_SIZE(preference_insert_targets), &insert,
					   &ignore, NULL, &result, &error),
			&error);
	if (rc != 0 || result.affected_rows)
		return rc;

	const struct norm_table_ref preferences =
		norm_table(&memory_preference_model, NB_STR_LIT("p"));
	const struct norm_join join = {
		.table = &scopes,
		.kind = NORM_JOIN_INNER,
		.on = norm_columns_eq(norm_table_col(&scopes, SCOPE_OWNER),
				      norm_table_col(&preferences, PREFERENCE_OWNER))};
	const struct norm_term saved_target =
		norm_term_column(norm_table_col(&preferences, PREFERENCE_TARGET));
	const struct norm_expr check[] = {
		norm_eq(norm_table_col(&scopes, SCOPE_SESSION_ID), norm_i64(session_id)),
		norm_columns_eq(norm_table_col(&preferences, PREFERENCE_SESSION_ID),
				norm_table_col(&scopes, SCOPE_SESSION_ID)),
		norm_eq(norm_table_col(&preferences, PREFERENCE_EVENT_TOKEN),
			orm_string(event_token)),
		norm_eq(norm_table_col(&preferences, PREFERENCE_SCOPE), orm_string(scope)),
		norm_eq(norm_table_col(&preferences, PREFERENCE_KEY), orm_string(key)),
		norm_compare_terms(&saved_target, NORM_COMPARE_EQ, &target),
	};
	const struct norm_query_field checked[] = {
		{.value = norm_term_column(norm_table_col(&preferences, PREFERENCE_KEY)),
		 .into = 0},
		{.value = norm_term_column(norm_table_col(&preferences, PREFERENCE_VALUE)),
		 .into = 1},
		{.value = norm_term_column(norm_table_col(&preferences, PREFERENCE_SCOPE)),
		 .into = 2},
		{.value = norm_term_column(norm_table_col(&preferences, PREFERENCE_ID)), .into = 3},
		{.value = norm_term_column(norm_table_col(&preferences, PREFERENCE_SOURCE)),
		 .into = 4},
	};
	const struct norm_query query = {.from = &preferences,
					 .joins = &join,
					 .join_count = 1,
					 .projection = &preference_model,
					 .fields = checked,
					 .field_count = 5,
					 .where = norm_and(check, NB_ARRAY_SIZE(check))};
	rc = orm_status(db, norm_query_select(lease.connection, &query, NULL, &rows, &error),
			&error);
	if (rc != 0)
		return rc;

	const struct preference_row *same = norm_rows_at(rows, 0);
	bool matches = same && (value ? !same->value_missing && nb_str_eq_cstr(same->value, value)
				      : same->value_missing);

	return matches ? 0 : NB_ERR_STALE;
}

struct preference_ranked_row {
	struct memory_preference_row item;
	int64_t version;
	int64_t rank;
};

enum { PREF_VERSION = PREFERENCE_CREATED_AT + 1, PREF_RANK, PREF_FIELDS };

static int preference_read(struct db *db, int64_t session_id, int history, struct norm_rows **rows)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_table_ref preferences =
		norm_table(&memory_preference_model, NB_STR_LIT("p"));
	const struct norm_table_ref scopes = norm_table(&memory_scope_model, NB_STR_LIT("s"));
	const struct norm_join join = {
		.table = &scopes,
		.kind = NORM_JOIN_INNER,
		.on = norm_columns_eq(norm_table_col(&scopes, SCOPE_OWNER),
				      norm_table_col(&preferences, PREFERENCE_OWNER))};
	const struct norm_term target =
		norm_term_column(norm_table_col(&preferences, PREFERENCE_TARGET));
	const struct norm_term project = norm_term_column(norm_table_col(&scopes, SCOPE_PROJECT));
	const struct norm_term session =
		norm_term_column(norm_table_col(&scopes, SCOPE_SESSION_ID));
	const struct norm_term session_text = norm_term_cast(&session, NORM_TYPE_TEXT);
	const struct norm_expr projects[] = {
		norm_eq(norm_table_col(&preferences, PREFERENCE_SCOPE), orm_string("project")),
		norm_compare_terms(&target, NORM_COMPARE_EQ, &project)};
	const struct norm_expr sessions[] = {
		norm_eq(norm_table_col(&preferences, PREFERENCE_SCOPE), orm_string("session")),
		norm_compare_terms(&target, NORM_COMPARE_EQ, &session_text)};
	const struct norm_expr applicable[] = {
		norm_eq(norm_table_col(&preferences, PREFERENCE_SCOPE), orm_string("personal")),
		norm_and(projects, 2), norm_and(sessions, 2)};
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&scopes, SCOPE_SESSION_ID), norm_i64(session_id)),
		norm_or(applicable, 3)};
	const size_t selected[] = {PREFERENCE_KEY, PREFERENCE_VALUE, PREFERENCE_SCOPE,
				   PREFERENCE_ID, PREFERENCE_SOURCE};
	struct norm_query_field history_fields[5] = {0};
	for (size_t i = 0; i < 5; i++)
		history_fields[i] = (struct norm_query_field){
			.value = norm_term_column(norm_table_col(&preferences, selected[i])),
			.into = i};

	const struct norm_query_order history_order = {
		.value = norm_term_column(norm_table_col(&preferences, PREFERENCE_ID)),
		.direction = NORM_DESC};
	const struct norm_result_options limits = {.max_rows = SIZE_MAX, .max_bytes = SIZE_MAX};
	struct norm_query query = {.from = &preferences,
				   .joins = &join,
				   .join_count = 1,
				   .projection = &preference_model,
				   .fields = history_fields,
				   .field_count = 5,
				   .where = norm_and(conditions, 2),
				   .order = &history_order,
				   .order_count = 1,
				   .result_options = &limits};

	struct norm_field ranked_fields[PREF_FIELDS] = {0};
	for (size_t i = 0; i < memory_preference_model.field_count; i++)
		ranked_fields[i] = memory_preference_model.fields[i];

	ranked_fields[PREF_VERSION] = (struct norm_field)NORM_FIELD(struct preference_ranked_row,
								    version, NORM_TYPE_I64, 0);
	ranked_fields[PREF_RANK] =
		(struct norm_field)NORM_FIELD(struct preference_ranked_row, rank, NORM_TYPE_I64, 0);
	const struct norm_model versions_model = {.table = NB_STR_LIT("versions"),
						  .size = sizeof(struct preference_ranked_row),
						  .alignment =
							  _Alignof(struct preference_ranked_row),
						  .fields = ranked_fields,
						  .field_count = PREF_RANK};
	const struct norm_model resolved_model = {.table = NB_STR_LIT("resolved"),
						  .size = sizeof(struct preference_ranked_row),
						  .alignment =
							  _Alignof(struct preference_ranked_row),
						  .fields = ranked_fields,
						  .field_count = PREF_FIELDS};
	const struct norm_table_ref versions = norm_table(&versions_model, NB_STR_LIT("v"));
	const struct norm_table_ref resolved = norm_table(&resolved_model, NB_STR_LIT("r"));
	struct norm_query_field version_fields[PREF_RANK] = {0};
	struct norm_query_field resolved_fields[PREF_FIELDS] = {0};
	for (size_t i = 0; i < PREF_VERSION; i++)
		version_fields[i] = (struct norm_query_field){
			.value = norm_term_column(norm_table_col(&preferences, i)), .into = i};

	const struct norm_term version_partition[] = {
		norm_term_column(norm_table_col(&preferences, PREFERENCE_SCOPE)),
		norm_term_column(norm_table_col(&preferences, PREFERENCE_TARGET)),
		norm_term_column(norm_table_col(&preferences, PREFERENCE_KEY))};
	const struct norm_query_order version_order[] = {
		{.value = norm_term_column(norm_table_col(&preferences, PREFERENCE_SOURCE_ORDER)),
		 .direction = NORM_DESC},
		{.value = norm_term_column(norm_table_col(&preferences, PREFERENCE_ID)),
		 .direction = NORM_DESC}};
	version_fields[PREF_VERSION] = (struct norm_query_field){
		.value = norm_term_row_number((struct norm_window){.partition = version_partition,
								   .partition_count = 3,
								   .order = version_order,
								   .order_count = 2}),
		.into = PREF_VERSION};
	const struct norm_query version_query = {.from = &preferences,
						 .joins = &join,
						 .join_count = 1,
						 .projection = &versions_model,
						 .fields = version_fields,
						 .field_count = PREF_RANK,
						 .where = norm_and(conditions, 2)};

	for (size_t i = 0; i < PREF_RANK; i++)
		resolved_fields[i] = (struct norm_query_field){
			.value = norm_term_column(norm_table_col(&versions, i)), .into = i};

	const struct norm_term zero = norm_term_value(norm_i64(0));
	const struct norm_term one = norm_term_value(norm_i64(1));
	const struct norm_term two = norm_term_value(norm_i64(2));
	const struct norm_term three = norm_term_value(norm_i64(3));
	const struct norm_case explicit_case[] = {
		{.when = norm_eq(norm_table_col(&versions, PREFERENCE_ORIGIN),
				 orm_string("explicit")),
		 .then = &one}};
	const struct norm_case scope_cases[] = {
		{.when = norm_eq(norm_table_col(&versions, PREFERENCE_SCOPE),
				 orm_string("session")),
		 .then = &three},
		{.when = norm_eq(norm_table_col(&versions, PREFERENCE_SCOPE),
				 orm_string("project")),
		 .then = &two}};
	const struct norm_query_order rank_order[] = {
		{.value = norm_term_case(explicit_case, 1, &zero), .direction = NORM_DESC},
		{.value = norm_term_case(scope_cases, 2, &one), .direction = NORM_DESC}};
	const struct norm_term rank_partition[] = {
		norm_term_column(norm_table_col(&versions, PREFERENCE_KEY))};
	resolved_fields[PREF_RANK] = (struct norm_query_field){
		.value = norm_term_row_number((struct norm_window){.partition = rank_partition,
								   .partition_count = 1,
								   .order = rank_order,
								   .order_count = 2}),
		.into = PREF_RANK};
	const struct norm_expr current[] = {
		norm_eq(norm_table_col(&versions, PREF_VERSION), norm_i64(1)),
		norm_is_not_null(norm_table_col(&versions, PREFERENCE_VALUE))};
	const struct norm_query resolved_query = {.from = &versions,
						  .projection = &resolved_model,
						  .fields = resolved_fields,
						  .field_count = PREF_FIELDS,
						  .where = norm_and(current, 2)};
	const struct norm_cte ctes[] = {{.model = &versions_model, .query = &version_query},
					{.model = &resolved_model, .query = &resolved_query}};
	struct norm_query_field effective_fields[5] = {0};
	for (size_t i = 0; i < 5; i++)
		effective_fields[i] = (struct norm_query_field){
			.value = norm_term_column(norm_table_col(&resolved, selected[i])),
			.into = i};

	const struct norm_case key_cases[] = {
		{.when = norm_eq(norm_table_col(&resolved, PREFERENCE_KEY),
				 orm_string("response.language")),
		 .then = &zero},
		{.when = norm_eq(norm_table_col(&resolved, PREFERENCE_KEY),
				 orm_string("response.detail")),
		 .then = &one}};
	const struct norm_query_order effective_order[] = {
		{.value = norm_term_case(key_cases, 2, &two), .direction = NORM_ASC},
		{.value = norm_term_column(norm_table_col(&resolved, PREFERENCE_KEY)),
		 .direction = NORM_ASC}};
	if (!history)
		query = (struct norm_query){
			.with = ctes,
			.with_count = 2,
			.from = &resolved,
			.projection = &preference_model,
			.fields = effective_fields,
			.field_count = 5,
			.where = norm_eq(norm_table_col(&resolved, PREF_RANK), norm_i64(1)),
			.order = effective_order,
			.order_count = 2,
			.result_options = &limits};

	int rc = orm_scope_begin(db, &lease);

	return rc ? rc
		  : orm_status(db, norm_query_select(lease.connection, &query, NULL, rows, &error),
			       &error);
}

int preference_visit_effective(struct db *db, int64_t session_id, preference_visit_fn visit,
			       void *data)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!visit)
		return NB_ERR_INVAL;

	int rc = preference_read(db, session_id, 0, &rows);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct preference_row *row = norm_rows_at(rows, i);
		char *key NB_CLEANUP(nb_charp_cleanup) = orm_text_copy(row->key);
		char *value NB_CLEANUP(nb_charp_cleanup) =
			row->value_missing ? NULL : orm_text_copy(row->value);
		char *scope NB_CLEANUP(nb_charp_cleanup) = orm_text_copy(row->scope);

		if (!key || (!row->value_missing && !value) || !scope)
			return NB_ERR_NOMEM;

		rc = visit(key, value, scope, row->id, data);
	}

	return rc;
}

char *preference_render_copy(struct db *db, int64_t session_id, int history)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	nb_buf buffer NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = preference_read(db, session_id, history, &rows);

	if (rc != 0)
		return NULL;

	for (size_t i = 0; i < norm_rows_count(rows); i++) {
		const struct preference_row *row = norm_rows_at(rows, i);
		char *key NB_CLEANUP(nb_charp_cleanup) = orm_text_copy(row->key);
		char *value NB_CLEANUP(nb_charp_cleanup) =
			row->value_missing ? NULL : orm_text_copy(row->value);
		char *scope NB_CLEANUP(nb_charp_cleanup) = orm_text_copy(row->scope);
		char *source NB_CLEANUP(nb_charp_cleanup) =
			row->source_missing ? NULL : orm_text_copy(row->source);

		if (!key || (!row->value_missing && !value) || !scope ||
		    (!row->source_missing && !source))
			return NULL;

		if (!nb_buf_printf(&buffer, "- %s: %s [%s; revision=%lld]\n", key,
				   value ? value : "(unset)", scope, (long long)row->id))
			return NULL;

		if (history && !nb_buf_printf(&buffer, "  source: %s\n", source ? source : ""))
			return NULL;
	}

	return nb_buf_ok(&buffer) ? orm_text_copy(nb_buf_str(&buffer)) : NULL;
}

int preference_clear_session(struct db *db, int64_t session_id)
{
	struct orm_scope lease NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_table_ref preferences =
		norm_table(&memory_preference_model, NB_STR_LIT("p"));
	struct preference_order_query order = {0};
	preference_order_query_init(&order, session_id, NULL);
	const struct norm_term session = norm_term_value(norm_i64(session_id));
	const struct norm_term target = norm_term_cast(&session, NORM_TYPE_TEXT);
	const struct norm_term random_size = norm_term_value(norm_i64(16));
	const struct norm_term random =
		norm_term_named_call(NB_STR_LIT("randomblob"), &random_size, 1);
	const struct norm_term hexadecimal = norm_term_named_call(NB_STR_LIT("hex"), &random, 1);
	const struct norm_query_field fields[] = {
		{.value = norm_term_column(norm_table_col(&preferences, PREFERENCE_OWNER)),
		 .into = 0},
		{.value = norm_term_column(norm_table_col(&preferences, PREFERENCE_SCOPE)),
		 .into = 1},
		{.value = norm_term_column(norm_table_col(&preferences, PREFERENCE_TARGET)),
		 .into = 2},
		{.value = norm_term_column(norm_table_col(&preferences, PREFERENCE_KEY)),
		 .into = 3},
		{.value = norm_term_value(norm_null()), .into = 4},
		{.value = norm_term_value(orm_string("session memory cleared")), .into = 5},
		{.value = session, .into = 6},
		{.value = norm_term_call(NORM_FUNCTION_LOWER, &hexadecimal, 1), .into = 7},
		{.value = norm_term_scalar(&order.queries[1]), .into = 8},
	};
	const struct norm_term old_target =
		norm_term_column(norm_table_col(&preferences, PREFERENCE_TARGET));
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&preferences, PREFERENCE_SCOPE), orm_string("session")),
		norm_compare_terms(&old_target, NORM_COMPARE_EQ, &target)};
	const struct norm_term group[] = {
		norm_term_column(norm_table_col(&preferences, PREFERENCE_OWNER)),
		norm_term_column(norm_table_col(&preferences, PREFERENCE_SCOPE)),
		norm_term_column(norm_table_col(&preferences, PREFERENCE_TARGET)),
		norm_term_column(norm_table_col(&preferences, PREFERENCE_KEY))};
	const struct norm_query query = {.from = &preferences,
					 .projection = &preference_insert_model,
					 .fields = fields,
					 .field_count = 9,
					 .where = norm_and(conditions, 2),
					 .group = group,
					 .group_count = 4};
	int rc = orm_scope_begin(db, &lease);

	return rc ? rc
		  : orm_status(db,
			       norm_insert_select(lease.connection, &memory_preference_model,
						  preference_insert_targets,
						  NB_ARRAY_SIZE(preference_insert_targets), &query,
						  NULL, NULL, NULL, &error),
			       &error);
}
