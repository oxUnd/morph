#include "business_store.h"
#include "records.h"
#include "session_models.h"
#include "history_models.h"
#include "memory_models.h"
#include <norm/query.h>
#include "errors.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct plan_row {
	int64_t session_id;
	nb_str state_json;
	bool state_json_missing;
};

static const struct norm_field plan_fields[] = {
	NORM_FIELD(struct plan_row, session_id, NORM_TYPE_I64, 0),
	NORM_NULLABLE_TEXT_FIELD(struct plan_row, state_json, state_json_missing, 0, 0),
};

static const struct norm_model plan_model =
	NORM_MODEL(struct plan_row, "session_plans", plan_fields);

struct activation_row {
	int64_t session_id;
	nb_str skill_name;
	bool skill_name_missing;
};

static const struct norm_field activation_fields[] = {
	NORM_FIELD(struct activation_row, session_id, NORM_TYPE_I64, 0),
	NORM_NULLABLE_TEXT_FIELD(struct activation_row, skill_name, skill_name_missing, 0, 0),
};

static const struct norm_model activation_model =
	NORM_MODEL(struct activation_row, "session_skill_activations", activation_fields);

struct history_context_row {
	nb_str kind;
	bool kind_missing;
	int32_t token_count;
	nb_str content;
	bool content_missing;
	nb_str payload_json;
	bool payload_json_missing;
};

static const struct norm_field history_context_fields[] = {
	NORM_NULLABLE_TEXT_FIELD(struct history_context_row, kind, kind_missing, 0, 0),
	NORM_FIELD(struct history_context_row, token_count, NORM_TYPE_I32, 0),
	NORM_NULLABLE_TEXT_FIELD(struct history_context_row, content, content_missing, 0, 0),
	NORM_NULLABLE_TEXT_FIELD(struct history_context_row, payload_json, payload_json_missing, 0,
				 0),
};

static const struct norm_model history_context_model =
	NORM_MODEL(struct history_context_row, "model_history_items", history_context_fields);

struct artifact_row {
	int64_t session_id;
	bool session_id_missing;
	int64_t id;
	nb_str kind;
	bool kind_missing;
	nb_str path;
	bool path_missing;
	nb_str tool_name;
	bool tool_name_missing;
};

static const struct norm_field artifact_fields[] = {
	NORM_FIELD(struct artifact_row, id, NORM_TYPE_I64, 0),
	NORM_NULLABLE_TEXT_FIELD(struct artifact_row, kind, kind_missing, 0, 0),
	NORM_NULLABLE_TEXT_FIELD(struct artifact_row, path, path_missing, 0, 0),
	NORM_NULLABLE_TEXT_FIELD(struct artifact_row, tool_name, tool_name_missing, 0, 0),
	NORM_NULLABLE_FIELD(struct artifact_row, session_id, session_id_missing, NORM_TYPE_I64, 0),
};

static const struct norm_model artifact_model =
	NORM_MODEL(struct artifact_row, "outputs", artifact_fields);

struct refresh_session_row {
	int64_t session_id;
	nb_str display_id;
	bool display_id_missing;
};

static const struct norm_field refresh_session_fields[] = {
	NORM_FIELD(struct refresh_session_row, session_id, NORM_TYPE_I64, 0),
	NORM_NULLABLE_TEXT_FIELD(struct refresh_session_row, display_id, display_id_missing, 0, 0),
};

static const struct norm_model refresh_session_model =
	NORM_MODEL(struct refresh_session_row, "memory_scopes", refresh_session_fields);

struct compaction_row {
	int64_t session_id;
	int64_t created_at;
	int64_t id;
	nb_str trigger_kind;
	bool trigger_kind_missing;
	nb_str status;
	bool status_missing;
	int32_t input_tokens;
	int32_t output_tokens;
	int32_t error_code;
};

static const struct norm_field compaction_fields[] = {
	NORM_NULLABLE_TEXT_FIELD(struct compaction_row, trigger_kind, trigger_kind_missing, 0, 0),
	NORM_NULLABLE_TEXT_FIELD(struct compaction_row, status, status_missing, 0, 0),
	NORM_FIELD(struct compaction_row, input_tokens, NORM_TYPE_I32, 0),
	NORM_FIELD(struct compaction_row, output_tokens, NORM_TYPE_I32, 0),
	NORM_FIELD(struct compaction_row, error_code, NORM_TYPE_I32, 0),
	NORM_FIELD(struct compaction_row, session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct compaction_row, created_at, NORM_TYPE_I64, 0),
	NORM_FIELD(struct compaction_row, id, NORM_TYPE_I64, 0),
};

static const struct norm_model compaction_model =
	NORM_MODEL(struct compaction_row, "history_compaction_attempts", compaction_fields);

struct process_job_row {
	nb_str process_id;
	bool process_id_missing;
	int64_t session_id;
	nb_str data_json;
	bool data_json_missing;
	int64_t updated_at;
};

static const struct norm_field process_job_fields[] = {
	NORM_TEXT_FIELD(struct process_job_row, process_id, SIZE_MAX, NORM_FIELD_PRIMARY_KEY),
	NORM_FIELD(struct process_job_row, session_id, NORM_TYPE_I64, 0),
	NORM_TEXT_FIELD(struct process_job_row, data_json, 0, 0),
	NORM_FIELD(struct process_job_row, updated_at, NORM_TYPE_I64, 0),
};

static const size_t process_job_fk_field = 1;
static const size_t process_job_reference = SESSION_ID;
static const struct norm_foreign_key process_job_foreign_keys[] = {
	{.name = NB_STR_LIT("process_jobs_session_fk"),
	 .fields = &process_job_fk_field,
	 .field_count = 1,
	 .references = &session_model,
	 .reference_fields = &process_job_reference,
	 .on_delete = NORM_FK_CASCADE},
};
static const struct norm_index process_job_indexes[] = {
	NORM_INDEX("process_jobs_session", 1),
};
static const struct norm_model process_job_model = NORM_MODEL_WITH(
	struct process_job_row, "process_jobs", process_job_fields,
	NORM_FOREIGN_KEYS(process_job_foreign_keys), NORM_INDEXES(process_job_indexes));

struct request_snapshot_row {
	int64_t session_id;
	nb_str turn_id;
	bool turn_id_missing;
	nb_str provider;
	nb_str model;
	nb_str input_json;
	int64_t created_at;
};
static const struct norm_field request_snapshot_fields[] = {
	NORM_FIELD(struct request_snapshot_row, session_id, NORM_TYPE_I64, NORM_FIELD_PRIMARY_KEY),
	NORM_NULLABLE_FIELD(struct request_snapshot_row, turn_id, turn_id_missing, NORM_TYPE_TEXT,
			    0),
	NORM_FIELD(struct request_snapshot_row, provider, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct request_snapshot_row, model, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct request_snapshot_row, input_json, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct request_snapshot_row, created_at, NORM_TYPE_I64, 0),
};
static const struct norm_model request_snapshot_model =
	NORM_MODEL(struct request_snapshot_row, "model_request_snapshots", request_snapshot_fields);

struct recent_input_row {
	nb_str content;
	bool content_missing;
	int64_t sequence_no;
};
static const struct norm_field recent_input_fields[] = {
	NORM_NULLABLE_FIELD(struct recent_input_row, content, content_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct recent_input_row, sequence_no, NORM_TYPE_I64, 0),
};
static const struct norm_model recent_input_model =
	NORM_MODEL(struct recent_input_row, "morph_recent_inputs", recent_input_fields);

struct refresh_job_row {
	int64_t session_id;
	int64_t generation;
	nb_str state;
	int64_t content_revision;
};
static const struct norm_field refresh_job_fields[] = {
	NORM_FIELD(struct refresh_job_row, session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct refresh_job_row, generation, NORM_TYPE_I64, 0),
	NORM_FIELD(struct refresh_job_row, state, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct refresh_job_row, content_revision, NORM_TYPE_I64, 0),
};
static const struct norm_model refresh_job_model =
	NORM_MODEL(struct refresh_job_row, "memory_jobs", refresh_job_fields);

static int copy_text(nb_str text, bool missing, char **out)
{
	if (missing) {
		*out = NULL;
		return 0;
	}

	*out = orm_text_copy(text);
	return *out ? 0 : NB_ERR_NOMEM;
}

void store_text_records_cleanup(nb_vec *records)
{
	for (size_t i = 0; i < records->len; i++)
		free(*(char **)nb_vec_at(records, i));

	nb_vec_destroy(records);
}

void store_history_bodies_cleanup(nb_vec *records)
{
	for (size_t i = 0; i < records->len; i++) {
		struct store_history_body *record = nb_vec_at(records, i);

		free(record->content);
		free(record->payload_json);
	}

	nb_vec_destroy(records);
}

void store_history_context_cleanup(nb_vec *records)
{
	for (size_t i = 0; i < records->len; i++) {
		struct store_history_context *record = nb_vec_at(records, i);

		free(record->kind);
		free(record->content);
		free(record->payload_json);
	}

	nb_vec_destroy(records);
}

void store_artifacts_cleanup(nb_vec *records)
{
	for (size_t i = 0; i < records->len; i++) {
		struct store_artifact *record = nb_vec_at(records, i);

		free(record->kind);
		free(record->path);
		free(record->tool_name);
	}

	nb_vec_destroy(records);
}

void store_refresh_sessions_cleanup(nb_vec *records)
{
	for (size_t i = 0; i < records->len; i++) {
		struct store_refresh_session *record = nb_vec_at(records, i);

		free(record->display_id);
	}

	nb_vec_destroy(records);
}

void store_compaction_status_cleanup(struct store_compaction_status *status)
{
	free(status->trigger_kind);
	free(status->status);
	memset(status, 0, sizeof(*status));
}

int plan_store_put(struct db *db, int64_t session_id, const char *state)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct plan_row row = {.session_id = session_id,
				     .state_json = nb_str_from_cstr(state),
				     .state_json_missing = !state};
	const size_t key = 0;
	const struct norm_term excluded = norm_term_excluded(norm_col(&plan_model, 1));
	const struct norm_assignment change = {.column = norm_col(&plan_model, 1),
					       .expression = &excluded};
	const struct norm_conflict conflict = {.action = NORM_CONFLICT_UPDATE,
					       .fields = &key,
					       .field_count = 1,
					       .set = &change,
					       .set_count = 1};
	const struct norm_insert_options options = {.conflict = &conflict};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_insert(scope.connection, &plan_model, &row, &options, NULL,
					   NULL, &error),
			       &error);
}

int plan_store_copy(struct db *db, int64_t session_id, char **state)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	struct norm_select_query query = {
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
		.where = norm_eq(norm_col(&plan_model, 0), norm_i64(session_id))};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(
			db, norm_select(scope.connection, &plan_model, &query, NULL, &rows, &error),
			&error);

	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return NB_ERR_NOT_FOUND;

	const struct plan_row *row = norm_rows_at(rows, 0);

	return copy_text(row->state_json, row->state_json_missing, state);
}

int activation_store_set(struct db *db, int64_t session_id, const char *name, int active)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	int rc = orm_scope_begin(db, &scope);

	if (rc != 0)
		return rc;

	if (active) {
		const struct activation_row row = {.session_id = session_id,
						   .skill_name = nb_str_from_cstr(name),
						   .skill_name_missing = !name};
		const struct norm_conflict conflict = {.action = NORM_CONFLICT_NOTHING};
		const struct norm_insert_options options = {.conflict = &conflict};

		return orm_status(db,
				  norm_insert(scope.connection, &activation_model, &row, &options,
					      NULL, NULL, &error),
				  &error);
	}

	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&activation_model, 0), norm_i64(session_id)),
		norm_eq(norm_col(&activation_model, 1), orm_string(name)),
	};
	const struct norm_delete_query query = {
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions))};

	return orm_status(
		db, norm_delete(scope.connection, &activation_model, &query, NULL, NULL, &error),
		&error);
}

int activation_store_list_copy(struct db *db, int64_t session_id, nb_vec *names)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	struct norm_order order = {.column = norm_col(&activation_model, 1)};
	struct norm_select_query query = {
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
		.where = norm_eq(norm_col(&activation_model, 0), norm_i64(session_id)),
		.order = &order,
		.order_count = 1,
	};
	int rc = orm_scope_begin(db, &scope);

	nb_vec_init(names, sizeof(char *));
	if (rc == 0)
		rc = orm_status(db,
				norm_select(scope.connection, &activation_model, &query, NULL,
					    &rows, &error),
				&error);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct activation_row *row = norm_rows_at(rows, i);
		char **entry = nb_vec_push(names);

		if (!entry) {
			rc = NB_ERR_NOMEM;
			break;
		}

		*entry = NULL;
		rc = copy_text(row->skill_name, row->skill_name_missing, entry);
	}

	if (rc != 0)
		store_text_records_cleanup(names);

	return rc;
}

int evidence_store_latest_user_copy(struct db *db, int64_t session_id, int64_t *id, char **content)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const size_t fields[] = {HIST_ID, HIST_CONTENT};
	const struct norm_value kinds[] = {orm_string("user_message"), orm_string("user_steer")};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&history_model, HIST_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_col(&history_model, HIST_ROLE), orm_string("user")),
		norm_eq(norm_col(&history_model, HIST_ACTIVE), norm_i32(1)),
		norm_in(norm_col(&history_model, HIST_KIND), kinds, NB_ARRAY_SIZE(kinds)),
	};
	const struct norm_order order = {.column = norm_col(&history_model, HIST_SEQUENCE_NO),
					 .direction = NORM_DESC};
	const struct norm_select_query query = {
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
		.order = &order,
		.order_count = 1,
		.has_limit = true,
		.limit = 1,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(
			db,
			norm_select(scope.connection, &history_model, &query, NULL, &rows, &error),
			&error);

	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return NB_ERR_NOT_FOUND;

	const struct history_row *row = norm_rows_at(rows, 0);

	*id = row->id;
	return copy_text(row->content, row->content_missing, content);
}

int request_snapshot_store_put(struct db *db, int64_t session_id, const char *turn_id,
			       const char *provider, const char *model, const char *input_json)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_query_field selected[] = {
		{norm_term_value(norm_i64(session_id)), 0},
		{norm_term_value(orm_string(turn_id)), 1},
		{norm_term_value(orm_string(provider)), 2},
		{norm_term_value(orm_string(model)), 3},
		{norm_term_value(orm_string(input_json)), 4},
		{norm_term_call(NORM_FUNCTION_CURRENT_EPOCH, NULL, 0), 5},
	};
	const size_t fields[] = {0, 1, 2, 3, 4, 5};
	const size_t key = 0;
	struct norm_term excluded[5] = {0};
	struct norm_assignment changes[5] = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(changes); i++) {
		excluded[i] = norm_term_excluded(norm_col(&request_snapshot_model, i + 1));
		changes[i] =
			(struct norm_assignment){.column = norm_col(&request_snapshot_model, i + 1),
						 .expression = &excluded[i]};
	}

	const struct norm_conflict conflict = {.action = NORM_CONFLICT_UPDATE,
					       .fields = &key,
					       .field_count = 1,
					       .set = changes,
					       .set_count = NB_ARRAY_SIZE(changes)};
	const struct norm_query query = {.projection = &request_snapshot_model,
					 .fields = selected,
					 .field_count = NB_ARRAY_SIZE(selected)};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_insert_select(scope.connection, &request_snapshot_model, fields,
						  NB_ARRAY_SIZE(fields), &query, &conflict, NULL,
						  NULL, &error),
			       &error);
}

int history_body_store_list_copy(struct db *db, int64_t session_id, const char *turn_id,
				 nb_vec *records)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const struct norm_table_ref users = norm_table(&history_model, NB_STR_LIT("u"));
	const struct norm_table_ref answers = norm_table(&history_model, NB_STR_LIT("a"));
	const struct norm_term answer_sequence =
		norm_term_column(norm_table_col(&answers, HIST_SEQUENCE_NO));
	const struct norm_term user_sequence =
		norm_term_column(norm_table_col(&users, HIST_SEQUENCE_NO));
	const struct norm_expr answered[] = {
		norm_columns_eq(norm_table_col(&answers, HIST_SESSION_ID),
				norm_table_col(&users, HIST_SESSION_ID)),
		norm_columns_eq(norm_table_col(&answers, HIST_TURN_ID),
				norm_table_col(&users, HIST_TURN_ID)),
		norm_eq(norm_table_col(&answers, HIST_KIND), orm_string("assistant_message")),
		norm_compare_terms(&answer_sequence, NORM_COMPARE_GT, &user_sequence),
	};
	const struct norm_query_field one = {norm_term_value(norm_i32(1)), 0};
	const struct norm_query answer_query = {
		.from = &answers,
		.projection = &orm_integer_model,
		.fields = &one,
		.field_count = 1,
		.where = norm_and(answered, NB_ARRAY_SIZE(answered)),
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&users, HIST_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_table_col(&users, HIST_KIND), orm_string("user_message")),
		turn_id ? norm_eq(norm_table_col(&users, HIST_TURN_ID), orm_string(turn_id))
			: norm_eq(norm_table_col(&users, HIST_ACTIVE), norm_i32(1)),
		norm_exists(&answer_query),
	};
	const size_t fields[] = {HIST_ID, HIST_CONTENT, HIST_PAYLOAD_JSON};
	struct norm_query_field selected[NB_ARRAY_SIZE(fields)] = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(fields); i++)
		selected[i] = (struct norm_query_field){
			norm_term_column(norm_table_col(&users, fields[i])), fields[i]};

	const struct norm_result_options limits = {
		.max_rows = SIZE_MAX, .max_bytes = SIZE_MAX, .max_row_bytes = SIZE_MAX};
	const struct norm_query query = {.from = &users,
					 .projection = &history_model,
					 .fields = selected,
					 .field_count = NB_ARRAY_SIZE(selected),
					 .where = norm_and(conditions, turn_id ? 3 : 4),
					 .result_options = &limits};
	int rc = orm_scope_begin(db, &scope);

	nb_vec_init(records, sizeof(struct store_history_body));
	if (rc == 0)
		rc = orm_status(db,
				norm_query_select(scope.connection, &query, NULL, &rows, &error),
				&error);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct history_row *row = norm_rows_at(rows, i);
		struct store_history_body *entry = nb_vec_push(records);

		if (!entry) {
			rc = NB_ERR_NOMEM;
			break;
		}

		memset(entry, 0, sizeof(*entry));
		entry->id = row->id;
		rc = copy_text(row->content, row->content_missing, &entry->content);
		if (rc == 0)
			rc = copy_text(row->payload_json, row->payload_json_missing,
				       &entry->payload_json);
	}

	if (rc != 0)
		store_history_bodies_cleanup(records);

	return rc;
}

int history_body_store_update(struct db *db, int64_t id, const char *payload_json, int tokens)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_assignment changes[] = {
		{.column = norm_col(&history_model, HIST_PAYLOAD_JSON),
		 .value = orm_string(payload_json)},
		{.column = norm_col(&history_model, HIST_TOKEN_COUNT), .value = norm_i32(tokens)},
	};
	const struct norm_update_query query = {
		.set = changes,
		.set_count = NB_ARRAY_SIZE(changes),
		.where = norm_eq(norm_col(&history_model, HIST_ID), norm_i64(id))};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_update(scope.connection, &history_model, &query, NULL, NULL,
					   &error),
			       &error);
}

int history_context_store_list_copy(struct db *db, int64_t session_id, nb_vec *records)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const struct norm_table_ref history = norm_table(&history_model, NB_STR_LIT("h"));
	const struct norm_expr receipt =
		norm_eq(norm_table_col(&history, HIST_KIND), orm_string("background_receipt"));
	const struct norm_term content = norm_term_column(norm_table_col(&history, HIST_CONTENT));
	const struct norm_term payload =
		norm_term_column(norm_table_col(&history, HIST_PAYLOAD_JSON));
	const struct norm_term empty = norm_term_value(norm_null());
	const struct norm_case content_case = {.when = receipt, .then = &content};
	const struct norm_case payload_case = {.when = receipt, .then = &payload};
	const struct norm_query_field selected[] = {
		{norm_term_column(norm_table_col(&history, HIST_KIND)), 0},
		{norm_term_column(norm_table_col(&history, HIST_TOKEN_COUNT)), 1},
		{norm_term_case(&content_case, 1, &empty), 2},
		{norm_term_case(&payload_case, 1, &empty), 3},
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&history, HIST_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_table_col(&history, HIST_ACTIVE), norm_i32(1)),
	};
	const struct norm_result_options limits = {
		.max_rows = SIZE_MAX, .max_bytes = SIZE_MAX, .max_row_bytes = SIZE_MAX};
	const struct norm_query query = {.from = &history,
					 .projection = &history_context_model,
					 .fields = selected,
					 .field_count = NB_ARRAY_SIZE(selected),
					 .where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
					 .result_options = &limits};
	int rc = orm_scope_begin(db, &scope);

	nb_vec_init(records, sizeof(struct store_history_context));
	if (rc == 0)
		rc = orm_status(db,
				norm_query_select(scope.connection, &query, NULL, &rows, &error),
				&error);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct history_context_row *row = norm_rows_at(rows, i);
		struct store_history_context *entry = nb_vec_push(records);

		if (!entry) {
			rc = NB_ERR_NOMEM;
			break;
		}

		memset(entry, 0, sizeof(*entry));
		entry->token_count = row->token_count;
		rc = copy_text(row->kind, row->kind_missing, &entry->kind);
		if (rc == 0)
			rc = copy_text(row->content, row->content_missing, &entry->content);

		if (rc == 0)
			rc = copy_text(row->payload_json, row->payload_json_missing,
				       &entry->payload_json);
	}

	if (rc != 0)
		store_history_context_cleanup(records);

	return rc;
}

static int text_records_copy(const struct norm_rows *rows, nb_vec *records)
{
	int rc = 0;

	nb_vec_init(records, sizeof(char *));
	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct orm_text *row = norm_rows_at(rows, i);
		char **entry = nb_vec_push(records);

		if (!entry) {
			rc = NB_ERR_NOMEM;
			break;
		}

		*entry = NULL;
		rc = copy_text(row->value, row->missing, entry);
	}

	if (rc != 0)
		store_text_records_cleanup(records);

	return rc;
}

int history_user_inputs_store_copy(struct db *db, int64_t session_id, const char *turn_id,
				   nb_vec *records)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const struct norm_table_ref history = norm_table(&history_model, NB_STR_LIT("h"));
	const struct norm_table_ref recent = norm_table(&recent_input_model, NB_STR_LIT("recent"));
	const struct norm_term source_path[] = {
		norm_term_column(norm_table_col(&history, HIST_PAYLOAD_JSON)),
		norm_term_value(orm_string("$.source")),
	};
	const struct norm_term source_arguments[] = {
		norm_term_call(NORM_FUNCTION_JSON_EXTRACT, source_path, NB_ARRAY_SIZE(source_path)),
		norm_term_value(orm_string("")),
	};
	const struct norm_term source = norm_term_call(NORM_FUNCTION_COALESCE, source_arguments,
						       NB_ARRAY_SIZE(source_arguments));
	const struct norm_term hook = norm_term_value(orm_string("hook"));
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&history, HIST_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_table_col(&history, HIST_TURN_ID), orm_string(turn_id)),
		norm_eq(norm_table_col(&history, HIST_ROLE), orm_string("user")),
		norm_eq(norm_table_col(&history, HIST_KIND), orm_string("user_message")),
		norm_eq(norm_table_col(&history, HIST_ACTIVE), norm_i32(1)),
		norm_compare_terms(&source, NORM_COMPARE_NE, &hook),
	};
	const struct norm_query_field recent_fields[] = {
		{norm_term_column(norm_table_col(&history, HIST_CONTENT)), 0},
		{norm_term_column(norm_table_col(&history, HIST_SEQUENCE_NO)), 1},
	};
	const struct norm_query_order latest = {
		.value = norm_term_column(norm_table_col(&history, HIST_SEQUENCE_NO)),
		.direction = NORM_DESC};
	const struct norm_query recent_query = {
		.from = &history,
		.projection = &recent_input_model,
		.fields = recent_fields,
		.field_count = NB_ARRAY_SIZE(recent_fields),
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
		.order = &latest,
		.order_count = 1,
		.has_limit = true,
		.limit = 4};
	const struct norm_cte cte = {.model = &recent_input_model, .query = &recent_query};
	const struct norm_query_field selected = {norm_term_column(norm_table_col(&recent, 0)), 0};
	const struct norm_query_order chronological = {
		.value = norm_term_column(norm_table_col(&recent, 1))};
	const struct norm_result_options limits = {
		.max_rows = SIZE_MAX, .max_bytes = SIZE_MAX, .max_row_bytes = SIZE_MAX};
	const struct norm_query query = {.with = &cte,
					 .with_count = 1,
					 .from = &recent,
					 .projection = &orm_text_model,
					 .fields = &selected,
					 .field_count = 1,
					 .order = &chronological,
					 .order_count = 1,
					 .result_options = &limits};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(db,
				norm_query_select(scope.connection, &query, NULL, &rows, &error),
				&error);

	return rc == 0 ? text_records_copy(rows, records) : rc;
}

int artifact_context_store_list_copy(struct db *db, int64_t session_id, nb_vec *records)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const size_t fields[] = {0, 1, 2, 3};
	const struct norm_order order[] = {
		{.column = norm_col(&artifact_model, 0), .direction = NORM_DESC}};
	const struct norm_select_query query = {
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.where = norm_eq(norm_col(&artifact_model, 4), norm_i64(session_id)),
		.order = order,
		.order_count = NB_ARRAY_SIZE(order),
		.has_limit = true,
		.limit = 8,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};
	int rc = orm_scope_begin(db, &scope);

	nb_vec_init(records, sizeof(struct store_artifact));
	if (rc == 0)
		rc = orm_status(
			db,
			norm_select(scope.connection, &artifact_model, &query, NULL, &rows, &error),
			&error);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct artifact_row *row = norm_rows_at(rows, i);
		struct store_artifact *entry = nb_vec_push(records);

		if (!entry) {
			rc = NB_ERR_NOMEM;
			break;
		}

		memset(entry, 0, sizeof(*entry));
		entry->id = row->id;
		rc = copy_text(row->kind, row->kind_missing, &entry->kind);
		if (rc == 0)
			rc = copy_text(row->path, row->path_missing, &entry->path);

		if (rc == 0)
			rc = copy_text(row->tool_name, row->tool_name_missing, &entry->tool_name);
	}

	if (rc != 0)
		store_artifacts_cleanup(records);

	return rc;
}

int memory_refresh_store_list_copy(struct db *db, int64_t session_id, nb_vec *records)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const struct norm_table_ref scopes = norm_table(&memory_scope_model, NB_STR_LIT("s"));
	const struct norm_table_ref current =
		norm_table(&memory_scope_model, NB_STR_LIT("current"));
	const struct norm_table_ref sessions = norm_table(&session_model, NB_STR_LIT("session"));
	const struct norm_table_ref summaries = norm_table(&memory_summary_model, NB_STR_LIT("m"));
	const struct norm_table_ref jobs = norm_table(&refresh_job_model, NB_STR_LIT("j"));
	const struct norm_join joins[] = {
		{.kind = NORM_JOIN_INNER,
		 .table = &current,
		 .on = norm_eq(norm_table_col(&current, SCOPE_SESSION_ID), norm_i64(session_id))},
		{.kind = NORM_JOIN_INNER,
		 .table = &sessions,
		 .on = norm_columns_eq(norm_table_col(&sessions, SESSION_ID),
				       norm_table_col(&scopes, SCOPE_SESSION_ID))},
	};
	const struct norm_expr shared_project[] = {
		norm_columns_eq(norm_table_col(&scopes, SCOPE_OWNER),
				norm_table_col(&current, SCOPE_OWNER)),
		norm_ne(norm_table_col(&current, SCOPE_PROJECT), orm_string("")),
		norm_columns_eq(norm_table_col(&scopes, SCOPE_PROJECT),
				norm_table_col(&current, SCOPE_PROJECT)),
	};
	const struct norm_expr eligible[] = {
		norm_eq(norm_table_col(&scopes, SCOPE_SESSION_ID), norm_i64(session_id)),
		norm_and(shared_project, NB_ARRAY_SIZE(shared_project)),
	};
	const struct norm_query_field one = {norm_term_value(norm_i32(1)), 0};
	const struct norm_expr summary_conditions[] = {
		norm_columns_eq(norm_table_col(&summaries, SUMMARY_SESSION_ID),
				norm_table_col(&scopes, SCOPE_SESSION_ID)),
		norm_columns_eq(norm_table_col(&summaries, SUMMARY_MEMORY_VERSION),
				norm_table_col(&scopes, SCOPE_GENERATION)),
		norm_columns_eq(norm_table_col(&summaries, SUMMARY_CONTENT_REVISION),
				norm_table_col(&scopes, SCOPE_CONTENT_REVISION)),
	};
	const struct norm_query summary_query = {
		.from = &summaries,
		.projection = &orm_integer_model,
		.fields = &one,
		.field_count = 1,
		.where = norm_and(summary_conditions, NB_ARRAY_SIZE(summary_conditions)),
	};
	const struct norm_value active_states[] = {orm_string("queued"), orm_string("running")};
	const struct norm_expr failed_job[] = {
		norm_eq(norm_table_col(&jobs, 2), orm_string("failed")),
		norm_columns_eq(norm_table_col(&jobs, 3),
				norm_table_col(&scopes, SCOPE_CONTENT_REVISION)),
	};
	const struct norm_expr pending_job[] = {
		norm_in(norm_table_col(&jobs, 2), active_states, NB_ARRAY_SIZE(active_states)),
		norm_and(failed_job, NB_ARRAY_SIZE(failed_job)),
	};
	const struct norm_expr job_conditions[] = {
		norm_columns_eq(norm_table_col(&jobs, 0),
				norm_table_col(&scopes, SCOPE_SESSION_ID)),
		norm_columns_eq(norm_table_col(&jobs, 1),
				norm_table_col(&scopes, SCOPE_GENERATION)),
		norm_or(pending_job, NB_ARRAY_SIZE(pending_job)),
	};
	const struct norm_query job_query = {
		.from = &jobs,
		.projection = &orm_integer_model,
		.fields = &one,
		.field_count = 1,
		.where = norm_and(job_conditions, NB_ARRAY_SIZE(job_conditions)),
	};
	const struct norm_table_ref sources[] = {
		norm_table(&fact_model, NB_STR_LIT("facts")),
		norm_table(&memory_candidate_model, NB_STR_LIT("candidates")),
		norm_table(&memory_episode_model, NB_STR_LIT("episodes")),
		norm_table(&memory_note_model, NB_STR_LIT("notes")),
	};
	const size_t source_session_fields[] = {FACT_SESSION_ID, CANDIDATE_SESSION_ID,
						EPISODE_SESSION_ID, NOTE_SESSION_ID};
	struct norm_query source_queries[NB_ARRAY_SIZE(sources)] = {0};
	struct norm_expr has_sources[NB_ARRAY_SIZE(sources)] = {0};

	for (size_t i = 0; i < NB_ARRAY_SIZE(sources); i++) {
		source_queries[i] = (struct norm_query){
			.from = &sources[i],
			.projection = &orm_integer_model,
			.fields = &one,
			.field_count = 1,
			.where = norm_columns_eq(
				norm_table_col(&sources[i], source_session_fields[i]),
				norm_table_col(&scopes, SCOPE_SESSION_ID)),
		};
		has_sources[i] = norm_exists(&source_queries[i]);
	}

	const struct norm_expr summarized = norm_exists(&summary_query);
	const struct norm_expr pending = norm_exists(&job_query);
	const struct norm_expr conditions[] = {
		norm_or(eligible, NB_ARRAY_SIZE(eligible)),
		norm_not(&summarized),
		norm_not(&pending),
		norm_or(has_sources, NB_ARRAY_SIZE(has_sources)),
	};
	const struct norm_term display_arguments[] = {
		norm_term_column(norm_table_col(&sessions, SESSION_DISPLAY_ID)),
		norm_term_value(orm_string("")),
	};
	const struct norm_query_field selected[] = {
		{norm_term_column(norm_table_col(&scopes, SCOPE_SESSION_ID)), 0},
		{norm_term_call(NORM_FUNCTION_COALESCE, display_arguments,
				NB_ARRAY_SIZE(display_arguments)),
		 1},
	};
	const struct norm_query_order order[] = {
		{.value = norm_term_column(norm_table_col(&sessions, SESSION_UPDATED_AT)),
		 .direction = NORM_DESC},
		{.value = norm_term_column(norm_table_col(&scopes, SCOPE_SESSION_ID)),
		 .direction = NORM_DESC},
	};
	const struct norm_result_options limits = {
		.max_rows = SIZE_MAX, .max_bytes = SIZE_MAX, .max_row_bytes = SIZE_MAX};
	const struct norm_query query = {
		.from = &scopes,
		.joins = joins,
		.join_count = NB_ARRAY_SIZE(joins),
		.projection = &refresh_session_model,
		.fields = selected,
		.field_count = NB_ARRAY_SIZE(selected),
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions)),
		.order = order,
		.order_count = NB_ARRAY_SIZE(order),
		.result_options = &limits,
	};
	int rc = orm_scope_begin(db, &scope);

	nb_vec_init(records, sizeof(struct store_refresh_session));
	if (rc == 0)
		rc = orm_status(db,
				norm_query_select(scope.connection, &query, NULL, &rows, &error),
				&error);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct refresh_session_row *row = norm_rows_at(rows, i);
		struct store_refresh_session *entry = nb_vec_push(records);

		if (!entry) {
			rc = NB_ERR_NOMEM;
			break;
		}

		memset(entry, 0, sizeof(*entry));
		entry->session_id = row->session_id;
		rc = copy_text(row->display_id, row->display_id_missing, &entry->display_id);
	}

	if (rc != 0)
		store_refresh_sessions_cleanup(records);

	return rc;
}

int session_title_store_update(struct db *db, int64_t session_id, const char *display_id,
			       const char *old_name, const char *title)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_assignment change = {.column = norm_col(&session_model, SESSION_NAME),
					       .value = orm_string(title)};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&session_model, SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_col(&session_model, SESSION_DISPLAY_ID), orm_string(display_id)),
		norm_eq(norm_col(&session_model, SESSION_NAME), orm_string(old_name)),
	};
	const struct norm_update_query query = {
		.set = &change,
		.set_count = 1,
		.where = norm_and(conditions, NB_ARRAY_SIZE(conditions))};
	int rc = orm_scope_begin(db, &scope);

	if (rc != 0)
		return rc;

	nb_status status =
		norm_update(scope.connection, &session_model, &query, NULL, NULL, &error);

	return status == NORM_ERR_UNIQUE || status == NORM_ERR_CONSTRAINT
		       ? NB_ERR_EXISTS
		       : orm_status(db, status, &error);
}

int compaction_status_store_copy(struct db *db, int64_t session_id,
				 struct store_compaction_status *status)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const size_t fields[] = {0, 1, 2, 3, 4};
	const struct norm_order order[] = {
		{.column = norm_col(&compaction_model, 6), .direction = NORM_DESC},
		{.column = norm_col(&compaction_model, 7), .direction = NORM_DESC}};
	const struct norm_select_query query = {
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.where = norm_eq(norm_col(&compaction_model, 5), norm_i64(session_id)),
		.order = order,
		.order_count = NB_ARRAY_SIZE(order),
		.has_limit = true,
		.limit = 1,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(db,
				norm_select(scope.connection, &compaction_model, &query, NULL,
					    &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return NB_ERR_NOT_FOUND;

	const struct compaction_row *row = norm_rows_at(rows, 0);

	memset(status, 0, sizeof(*status));
	status->input_tokens = row->input_tokens;
	status->output_tokens = row->output_tokens;
	status->error_code = row->error_code;
	rc = copy_text(row->trigger_kind, row->trigger_kind_missing, &status->trigger_kind);
	if (rc == 0)
		rc = copy_text(row->status, row->status_missing, &status->status);

	if (rc != 0)
		store_compaction_status_cleanup(status);

	return rc;
}

int process_job_store_init(struct db *db)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_schema_options options = {.if_not_exists = true};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_schema_create(scope.connection, &process_job_model, &options,
						  NULL, &error),
			       &error);
}

int process_job_store_put(struct db *db, const char *process_id, int64_t session_id,
			  const char *data_json)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_query_field selected[] = {
		{norm_term_value(orm_string(process_id)), 0},
		{norm_term_value(norm_i64(session_id)), 1},
		{norm_term_value(orm_string(data_json)), 2},
		{norm_term_call(NORM_FUNCTION_CURRENT_EPOCH, NULL, 0), 3},
	};
	const struct norm_term excluded[] = {
		norm_term_excluded(norm_col(&process_job_model, 2)),
		norm_term_excluded(norm_col(&process_job_model, 3)),
	};
	const struct norm_assignment changes[] = {
		{.column = norm_col(&process_job_model, 2), .expression = &excluded[0]},
		{.column = norm_col(&process_job_model, 3), .expression = &excluded[1]},
	};
	const struct norm_term current_session = norm_term_column(norm_col(&process_job_model, 1));
	const struct norm_term inserted_session =
		norm_term_excluded(norm_col(&process_job_model, 1));
	const size_t key = 0;
	const size_t fields[] = {0, 1, 2, 3};
	const struct norm_conflict conflict = {
		.action = NORM_CONFLICT_UPDATE,
		.fields = &key,
		.field_count = 1,
		.set = changes,
		.set_count = NB_ARRAY_SIZE(changes),
		.where = norm_compare_terms(&current_session, NORM_COMPARE_EQ, &inserted_session)};
	const struct norm_query query = {.projection = &process_job_model,
					 .fields = selected,
					 .field_count = NB_ARRAY_SIZE(selected)};
	int rc = orm_scope_begin(db, &scope);

	return rc ? rc
		  : orm_status(db,
			       norm_insert_select(scope.connection, &process_job_model, fields,
						  NB_ARRAY_SIZE(fields), &query, &conflict, NULL,
						  NULL, &error),
			       &error);
}

int process_job_store_list_copy(struct db *db, int64_t session_id, nb_vec *records)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	struct norm_order order = {.column = norm_col(&process_job_model, 3),
				   .direction = NORM_DESC};
	struct norm_select_query query = {
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
		.where = norm_eq(norm_col(&process_job_model, 1), norm_i64(session_id)),
		.order = &order,
		.order_count = 1,
		.has_limit = true,
		.limit = 200,
	};
	int rc = orm_scope_begin(db, &scope);

	nb_vec_init(records, sizeof(char *));
	if (rc == 0)
		rc = orm_status(db,
				norm_select(scope.connection, &process_job_model, &query, NULL,
					    &rows, &error),
				&error);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(rows); i++) {
		const struct process_job_row *row = norm_rows_at(rows, i);
		char **entry = nb_vec_push(records);

		if (!entry) {
			rc = NB_ERR_NOMEM;
			break;
		}

		*entry = NULL;
		rc = copy_text(row->data_json, row->data_json_missing, entry);
	}

	if (rc != 0)
		store_text_records_cleanup(records);

	return rc;
}

int process_job_store_copy(struct db *db, int64_t session_id, const char *process_id,
			   char **data_json)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&process_job_model, 0), orm_string(process_id)),
		norm_eq(norm_col(&process_job_model, 1), norm_i64(session_id)),
	};
	struct norm_select_query query = {
		.max_rows = SIZE_MAX, .max_bytes = SIZE_MAX, .where = norm_and(conditions, 2)};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(db,
				norm_select(scope.connection, &process_job_model, &query, NULL,
					    &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return NB_ERR_NOT_FOUND;

	const struct process_job_row *row = norm_rows_at(rows, 0);

	return copy_text(row->data_json, row->data_json_missing, data_json);
}
