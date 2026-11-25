#include "credit_store.h"
#include "records.h"
#include "errors.h"
#include <norm/query.h>
#include <time.h>

struct credit_event_row {
	nb_str user_id;
	bool user_id_missing;
	nb_str session_id;
	bool session_id_missing;
	nb_str kind;
	nb_str provider;
	bool provider_missing;
	nb_str model;
	bool model_missing;
	int64_t input_tokens;
	int64_t output_tokens;
	int64_t image_units;
	int64_t video_seconds;
	double estimated_cost;
	nb_str currency;
	int64_t credits;
	nb_str metadata_json;
	bool metadata_json_missing;
	int64_t created_at;
};

static const struct norm_field credit_event_fields[] = {
	NORM_NULLABLE_FIELD(struct credit_event_row, user_id, user_id_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct credit_event_row, session_id, session_id_missing, NORM_TYPE_TEXT,
			    0),
	NORM_FIELD(struct credit_event_row, kind, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct credit_event_row, provider, provider_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct credit_event_row, model, model_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct credit_event_row, input_tokens, NORM_TYPE_I64, 0),
	NORM_FIELD(struct credit_event_row, output_tokens, NORM_TYPE_I64, 0),
	NORM_FIELD(struct credit_event_row, image_units, NORM_TYPE_I64, 0),
	NORM_FIELD(struct credit_event_row, video_seconds, NORM_TYPE_I64, 0),
	NORM_FIELD(struct credit_event_row, estimated_cost, NORM_TYPE_F64, 0),
	NORM_FIELD(struct credit_event_row, currency, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct credit_event_row, credits, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct credit_event_row, metadata_json, metadata_json_missing,
			    NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct credit_event_row, created_at, NORM_TYPE_I64, 0),
};

static const struct norm_model credit_event_model =
	NORM_MODEL(struct credit_event_row, "credit_events", credit_event_fields);

static const struct norm_field credit_summary_fields[] = {
	NORM_FIELD(struct credit_store_summary, credits, NORM_TYPE_I64, 0),
	NORM_FIELD(struct credit_store_summary, estimated_cost, NORM_TYPE_F64, 0),
	NORM_FIELD(struct credit_store_summary, event_count, NORM_TYPE_I32, 0),
};

static const struct norm_model credit_summary_model =
	NORM_MODEL(struct credit_store_summary, "", credit_summary_fields);

int credit_store_init_schema(struct db *db)
{
	return db_init_credit_schema(db);
}

int credit_store_record_event(struct db *db, const struct credit_store_event *event)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};

	if (!event || !event->kind)
		return NB_ERR_INVAL;

	if (!event->currency)
		return MORPH_ERR_DB;

	struct credit_event_row row = {
		.user_id = nb_str_from_cstr(event->user_id),
		.user_id_missing = !event->user_id,
		.session_id = nb_str_from_cstr(event->session_id),
		.session_id_missing = !event->session_id,
		.kind = nb_str_from_cstr(event->kind),
		.provider = nb_str_from_cstr(event->provider),
		.provider_missing = !event->provider,
		.model = nb_str_from_cstr(event->model),
		.model_missing = !event->model,
		.input_tokens = event->input_tokens,
		.output_tokens = event->output_tokens,
		.image_units = event->image_units,
		.video_seconds = event->video_seconds,
		.estimated_cost = event->estimated_cost,
		.currency = nb_str_from_cstr(event->currency),
		.credits = event->credits,
		.metadata_json = nb_str_from_cstr(event->metadata_json),
		.metadata_json_missing = !event->metadata_json,
		.created_at = (int64_t)time(NULL),
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_status(db,
				norm_insert(scope.connection, &credit_event_model, &row, NULL,
					    orm_scope_call(&scope), NULL, &error),
				&error);

	return rc;
}

static int credit_store_summary_query(struct db *db, size_t identity, const char *value, bool today,
				      struct credit_store_summary *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!value || !out)
		return NB_ERR_INVAL;

	memset(out, 0, sizeof(*out));
	struct norm_table_ref events = norm_table(&credit_event_model, NB_STR_LIT("event"));
	const struct norm_term credit = norm_term_column(norm_table_col(&events, 11));
	const struct norm_term cost = norm_term_column(norm_table_col(&events, 9));
	const struct norm_term credits[] = {norm_term_call(NORM_FUNCTION_SUM, &credit, 1),
					    norm_term_value(norm_i64(0))};
	const struct norm_term costs[] = {norm_term_call(NORM_FUNCTION_SUM, &cost, 1),
					  norm_term_value(norm_f64(0))};
	const struct norm_query_field fields[] = {
		{.value = norm_term_call(NORM_FUNCTION_COALESCE, credits, NB_ARRAY_SIZE(credits)),
		 .into = 0},
		{.value = norm_term_call(NORM_FUNCTION_COALESCE, costs, NB_ARRAY_SIZE(costs)),
		 .into = 1},
		{.value = norm_term_call(NORM_FUNCTION_COUNT, NULL, 0), .into = 2},
	};
	const struct norm_term day = norm_term_call(NORM_FUNCTION_UTC_DAY, NULL, 0);
	const struct norm_term created = norm_term_column(norm_table_col(&events, 13));
	const struct norm_expr conditions[] = {
		norm_eq(norm_table_col(&events, identity), orm_string(value)),
		norm_compare_terms(&created, NORM_COMPARE_GE, &day),
	};
	const struct norm_query query = {
		.from = &events,
		.projection = &credit_summary_model,
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.where = norm_and(conditions, today ? 2 : 1),
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_read(&scope, &query, &rows);

	if (rc == 0 && norm_rows_count(rows))
		*out = *(const struct credit_store_summary *)norm_rows_at(rows, 0);

	return rc;
}

int credit_store_summary_today(struct db *db, const char *user_id, struct credit_store_summary *out)
{
	return credit_store_summary_query(db, 0, user_id, true, out);
}

int credit_store_summary_total(struct db *db, const char *user_id, struct credit_store_summary *out)
{
	return credit_store_summary_query(db, 0, user_id, false, out);
}

int credit_store_summary_session(struct db *db, const char *session_id,
				 struct credit_store_summary *out)
{
	return credit_store_summary_query(db, 1, session_id, false, out);
}
