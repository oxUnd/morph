#include <norm/query.h>
#include "persistence/orm/fcgi_store.h"
#include "persistence/orm/fcgi_quota.h"
#include "sapi/fastcgi/session_store_internal.h"
#include "sapi/fastcgi/security.h"
#include "persistence/orm/fcgi_models.h"
#include "persistence/orm/fcgi_quota_internal.h"
#include "persistence/orm/records.h"
#include "cJSON.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const struct norm_field quota_fields[] = {
	NORM_FIELD(struct orm_fcgi_quota_view, profile, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct orm_fcgi_quota_view, daily_turns, NORM_TYPE_I64, 0),
	NORM_FIELD(struct orm_fcgi_quota_view, daily_images, NORM_TYPE_I64, 0),
	NORM_FIELD(struct orm_fcgi_quota_view, daily_videos, NORM_TYPE_I64, 0),
	NORM_FIELD(struct orm_fcgi_quota_view, storage_bytes, NORM_TYPE_I64, 0),
	NORM_FIELD(struct orm_fcgi_quota_view, max_concurrent_turns, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct orm_fcgi_quota_view, used_turns, missing_used_turns,
			    NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct orm_fcgi_quota_view, used_images, missing_used_images,
			    NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct orm_fcgi_quota_view, used_videos, missing_used_videos,
			    NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct orm_fcgi_quota_view, used_bytes, missing_used_bytes,
			    NORM_TYPE_I64, 0),
};
static const struct norm_model quota_model = {
	.size = sizeof(struct orm_fcgi_quota_view),
	.alignment = _Alignof(struct orm_fcgi_quota_view),
	.fields = quota_fields,
	.field_count = sizeof(quota_fields) / sizeof(quota_fields[0]),
};

struct quota_credit_view {
	int64_t credits;
	double cost;
	int64_t events;
	nb_str currency;
};
static const struct norm_field credit_fields[] = {
	NORM_FIELD(struct quota_credit_view, credits, NORM_TYPE_I64, 0),
	NORM_FIELD(struct quota_credit_view, cost, NORM_TYPE_F64, 0),
	NORM_FIELD(struct quota_credit_view, events, NORM_TYPE_I64, 0),
	NORM_FIELD(struct quota_credit_view, currency, NORM_TYPE_TEXT, 0),
};
static const struct norm_model credit_model = {
	.size = sizeof(struct quota_credit_view),
	.alignment = _Alignof(struct quota_credit_view),
	.fields = credit_fields,
	.field_count = sizeof(credit_fields) / sizeof(credit_fields[0]),
};

struct quota_credit_row {
	int64_t credits;
	double estimated_cost;
	nb_str currency;
	nb_str user_id;
	int64_t created_at;
};
static const struct norm_field quota_credit_fields[] = {
	NORM_FIELD(struct quota_credit_row, credits, NORM_TYPE_I64, 0),
	NORM_FIELD(struct quota_credit_row, estimated_cost, NORM_TYPE_F64, 0),
	NORM_FIELD(struct quota_credit_row, currency, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct quota_credit_row, user_id, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct quota_credit_row, created_at, NORM_TYPE_I64, 0),
};
static const struct norm_model quota_credit_model =
	NORM_MODEL(struct quota_credit_row, "credit_events", quota_credit_fields);

int orm_fcgi_quota_copy(struct orm_scope *scope, const char *user_id, struct norm_rows **rows)
{
	char day[11];
	struct tm utc = {0};
	time_t now = time(NULL);
	struct norm_error error = {0};

	if (!gmtime_r(&now, &utc) || strftime(day, sizeof(day), "%Y-%m-%d", &utc) != 10)
		return NB_ERR_IO;

	const struct norm_table_ref quota = norm_table(&orm_fcgi_user_quota, NB_STR_LIT("q"));
	const struct norm_table_ref profile = norm_table(&orm_fcgi_quota_profiles, NB_STR_LIT("p"));
	const struct norm_table_ref daily = norm_table(&orm_fcgi_usage_daily, NB_STR_LIT("d"));
	const struct norm_table_ref storage = norm_table(&orm_fcgi_usage_storage, NB_STR_LIT("st"));
	const struct norm_expr day_predicates[] = {
		norm_columns_eq(norm_table_col(&daily, 0), norm_table_col(&quota, 0)),
		norm_eq(norm_table_col(&daily, 1), orm_string(day)),
	};
	const struct norm_join joins[] = {
		{.kind = NORM_JOIN_INNER,
		 .table = &profile,
		 .on = norm_columns_eq(norm_table_col(&profile, 0), norm_table_col(&quota, 1))},
		{.kind = NORM_JOIN_LEFT, .table = &daily, .on = norm_and(day_predicates, 2)},
		{.kind = NORM_JOIN_LEFT,
		 .table = &storage,
		 .on = norm_columns_eq(norm_table_col(&storage, 0), norm_table_col(&quota, 0))},
	};
	const struct norm_join_column columns[] = {
		{.column = norm_table_col(&quota, 1), .into = 0},
		{.column = norm_table_col(&profile, 1), .into = 1},
		{.column = norm_table_col(&profile, 2), .into = 2},
		{.column = norm_table_col(&profile, 3), .into = 3},
		{.column = norm_table_col(&profile, 4), .into = 4},
		{.column = norm_table_col(&profile, 5), .into = 5},
		{.column = norm_table_col(&daily, 2), .into = 6},
		{.column = norm_table_col(&daily, 3), .into = 7},
		{.column = norm_table_col(&daily, 4), .into = 8},
		{.column = norm_table_col(&storage, 1), .into = 9},
	};
	const struct norm_join_query query = {
		.from = &quota,
		.joins = joins,
		.join_count = NB_ARRAY_SIZE(joins),
		.projection = &quota_model,
		.columns = columns,
		.column_count = NB_ARRAY_SIZE(columns),
		.select = {.where = norm_eq(norm_table_col(&quota, 0), orm_string(user_id))},
	};

	return orm_status(
		scope->database,
		norm_join_select(scope->connection, &query, orm_scope_call(scope), rows, &error),
		&error);
}

int orm_fcgi_usage_add(struct orm_scope *scope, const char *user_id, size_t counter, int64_t amount)
{
	char day[11];
	struct tm utc = {0};
	time_t now = time(NULL);
	struct norm_error error = {0};

	if (counter < 2 || counter > 4)
		return NB_ERR_INVAL;

	if (!gmtime_r(&now, &utc) || strftime(day, sizeof(day), "%Y-%m-%d", &utc) != 10)
		return NB_ERR_IO;

	const struct orm_fcgi_usage_daily_row row = {
		.user_id = nb_str_from_cstr(user_id),
		.day = nb_str_from_cstr(day),
		.turns = counter == 2 ? amount : 0,
		.images = counter == 3 ? amount : 0,
		.videos = counter == 4 ? amount : 0,
	};
	const struct norm_term current = norm_term_column(norm_col(&orm_fcgi_usage_daily, counter));
	const struct norm_term incoming =
		norm_term_excluded(norm_col(&orm_fcgi_usage_daily, counter));
	const struct norm_term added = norm_term_binary(NORM_BINARY_ADD, &current, &incoming);
	const struct norm_assignment set[] = {
		{.column = norm_col(&orm_fcgi_usage_daily, counter), .expression = &added},
	};
	const size_t keys[] = {0, 1};
	const size_t fields[] = {0, 1, counter};
	const struct norm_conflict conflict = {
		.action = NORM_CONFLICT_UPDATE,
		.fields = keys,
		.field_count = 2,
		.set = set,
		.set_count = 1,
	};
	const struct norm_insert_options options = {
		.fields = fields,
		.field_count = 3,
		.conflict = &conflict,
	};

	return orm_status(scope->database,
			  norm_insert(scope->connection, &orm_fcgi_usage_daily, &row, &options,
				      orm_scope_call(scope), NULL, &error),
			  &error);
}

int orm_fcgi_storage_add(struct orm_scope *scope, const char *user_id, int64_t amount)
{
	const struct orm_fcgi_usage_storage_row row = {.user_id = nb_str_from_cstr(user_id),
						       .used_bytes = amount};
	const struct norm_term current = norm_term_column(norm_col(&orm_fcgi_usage_storage, 1));
	const struct norm_term incoming = norm_term_excluded(norm_col(&orm_fcgi_usage_storage, 1));
	const struct norm_term added = norm_term_binary(NORM_BINARY_ADD, &current, &incoming);
	const struct norm_assignment set[] = {
		{.column = norm_col(&orm_fcgi_usage_storage, 1), .expression = &added},
	};
	const size_t keys[] = {0};
	const struct norm_conflict conflict = {
		.action = NORM_CONFLICT_UPDATE,
		.fields = keys,
		.field_count = 1,
		.set = set,
		.set_count = 1,
	};
	const struct norm_insert_options options = {.conflict = &conflict};
	struct norm_error error = {0};

	return orm_status(scope->database,
			  norm_insert(scope->connection, &orm_fcgi_usage_storage, &row, &options,
				      orm_scope_call(scope), NULL, &error),
			  &error);
}

static void add_limit_obj(cJSON *parent, const char *name, int64_t used, int64_t limit)
{
	cJSON *object = cJSON_CreateObject();

	cJSON_AddNumberToObject(object, "used", (double)used);
	cJSON_AddNumberToObject(object, "limit", (double)limit);
	cJSON_AddItemToObject(parent, name, object);
}

int store_user_quota_json(struct session_store *store, const char *user_id, char **out_json)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_rows *credit_rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!store || !user_id || !out_json)
		return NB_ERR_INVAL;

	int rc = orm_scope_begin(&store->db, &scope);

	if (rc == 0)
		rc = orm_fcgi_quota_copy(&scope, user_id, &rows);

	if (rc != 0)
		return rc;

	const struct orm_fcgi_quota_view *row = norm_rows_at(rows, 0);

	if (!row)
		return NB_ERR_NOT_FOUND;

	root = cJSON_CreateObject();
	cJSON *daily = root ? cJSON_AddObjectToObject(root, "daily") : NULL;
	cJSON *storage = root ? cJSON_AddObjectToObject(root, "storage") : NULL;
	cJSON *concurrent = root ? cJSON_AddObjectToObject(root, "concurrent_turns") : NULL;
	cJSON *credits = root ? cJSON_AddObjectToObject(root, "credits") : NULL;

	if (!root || !daily || !storage || !concurrent || !credits)
		return NB_ERR_NOMEM;

	cJSON_AddStringToObject(root, "profile", row->profile.ptr);
	add_limit_obj(daily, "turns", row->used_turns, row->daily_turns);
	add_limit_obj(daily, "images", row->used_images, row->daily_images);
	add_limit_obj(daily, "videos", row->used_videos, row->daily_videos);
	cJSON_AddNumberToObject(storage, "used_bytes", (double)row->used_bytes);
	cJSON_AddNumberToObject(storage, "limit_bytes", (double)row->storage_bytes);
	cJSON_AddNumberToObject(concurrent, "used", 0);
	cJSON_AddNumberToObject(concurrent, "limit", (double)row->max_concurrent_turns);
	const struct norm_table_ref source = norm_table(&quota_credit_model, NB_STR_LIT("c"));
	const struct norm_term credits_column = norm_term_column(norm_table_col(&source, 0));
	const struct norm_term cost_column = norm_term_column(norm_table_col(&source, 1));
	const struct norm_term currency_column = norm_term_column(norm_table_col(&source, 2));
	const struct norm_term credits_default[] = {
		norm_term_call(NORM_FUNCTION_SUM, &credits_column, 1),
		norm_term_value(norm_i64(0)),
	};
	const struct norm_term cost_default[] = {
		norm_term_call(NORM_FUNCTION_SUM, &cost_column, 1),
		norm_term_value(norm_f64(0)),
	};
	const struct norm_term currency_default[] = {
		norm_term_call(NORM_FUNCTION_MAX, &currency_column, 1),
		norm_term_value(orm_string("USD")),
	};
	const struct norm_query_field fields[] = {
		{.value = norm_term_call(NORM_FUNCTION_COALESCE, credits_default, 2), .into = 0},
		{.value = norm_term_call(NORM_FUNCTION_COALESCE, cost_default, 2), .into = 1},
		{.value = norm_term_call(NORM_FUNCTION_COUNT, NULL, 0), .into = 2},
		{.value = norm_term_call(NORM_FUNCTION_COALESCE, currency_default, 2), .into = 3},
	};
	int64_t now = fcgi_store_now();
	const struct norm_expr predicates[] = {
		norm_eq(norm_table_col(&source, 3), orm_string(user_id)),
		norm_ge(norm_table_col(&source, 4), norm_i64(now - now % 86400)),
	};
	const struct norm_query query = {
		.from = &source,
		.projection = &credit_model,
		.fields = fields,
		.field_count = 4,
		.where = norm_and(predicates, 2),
	};
	struct norm_error error = {0};

	rc = orm_status(&store->db,
			norm_query_select(scope.connection, &query, orm_scope_call(&scope),
					  &credit_rows, &error),
			&error);
	const struct quota_credit_view *credit = rc == 0 ? norm_rows_at(credit_rows, 0) : NULL;

	if (credit) {
		cJSON_AddNumberToObject(credits, "used_today", (double)credit->credits);
		cJSON_AddNumberToObject(credits, "estimated_cost_today", credit->cost);
		cJSON_AddNumberToObject(credits, "events_today", (double)credit->events);
		cJSON_AddStringToObject(credits, "currency", credit->currency.ptr);
	}

	cJSON_AddNumberToObject(credits, "daily_limit", -1);
	cJSON_AddBoolToObject(credits, "over_limit", 0);
	char *json = cJSON_PrintUnformatted(root);

	if (!json)
		return NB_ERR_NOMEM;

	*out_json = json;
	return 0;
}

static int begin_turn_write(struct session_store *store, const char *user_id,
			    const char *session_id, const char *turn_id)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_rows *running_rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	int rc = orm_scope_begin(&store->db, &scope);

	if (rc == 0)
		rc = orm_fcgi_quota_copy(&scope, user_id, &rows);

	if (rc != 0)
		return rc;

	const struct orm_fcgi_quota_view *quota = norm_rows_at(rows, 0);

	if (!quota)
		return NB_ERR_NOT_FOUND;

	const struct norm_table_ref running_source =
		norm_table(&orm_fcgi_running_turns, NB_STR_LIT("r"));
	const struct norm_query_field count_fields[] = {
		{.value = norm_term_call(NORM_FUNCTION_COUNT, NULL, 0), .into = 0},
	};
	const struct norm_query count_query = {
		.from = &running_source,
		.projection = &orm_integer_model,
		.fields = count_fields,
		.field_count = 1,
		.where = norm_eq(norm_table_col(&running_source, 1), orm_string(user_id)),
	};
	struct norm_error count_error = {0};

	rc = orm_status(&store->db,
			norm_query_select(scope.connection, &count_query, orm_scope_call(&scope),
					  &running_rows, &count_error),
			&count_error);
	if (rc != 0)
		return rc;

	const struct orm_integer *running = norm_rows_at(running_rows, 0);

	if (!running)
		return NB_ERR_IO;

	if (quota->daily_turns >= 0 && quota->used_turns >= quota->daily_turns)
		return NB_ERR_NO_SPACE;

	if (quota->max_concurrent_turns >= 0 && running->value >= quota->max_concurrent_turns)
		return NB_ERR_AGAIN;

	rc = orm_fcgi_usage_add(&scope, user_id, 2, 1);
	int64_t now = fcgi_store_now();
	const struct orm_fcgi_running_turns_row turn = {
		.id = nb_str_from_cstr(turn_id),
		.user_id = nb_str_from_cstr(user_id),
		.session_id = nb_str_from_cstr(session_id),
		.started_at = now,
		.heartbeat_at = now,
	};
	struct norm_error error = {0};

	if (rc == 0)
		rc = orm_status(&store->db,
				norm_insert(scope.connection, &orm_fcgi_running_turns, &turn, NULL,
					    orm_scope_call(&scope), NULL, &error),
				&error);

	return rc;
}

int store_quota_begin_turn(struct session_store *store, const char *user_id, const char *session_id,
			   char out_turn_id[64])
{
	char turn_id[64];

	if (!store || !user_id || !session_id)
		return NB_ERR_INVAL;

	int rc = fcgi_random_id("turn_", turn_id, sizeof(turn_id));

	if (rc == 0)
		rc = fcgi_store_transaction_begin(store);

	if (rc != 0)
		return rc;

	rc = fcgi_store_transaction_end(store,
					begin_turn_write(store, user_id, session_id, turn_id));
	if (rc == 0 && out_turn_id)
		rc = nb_str_copy_to(nb_str_from_cstr(turn_id), out_turn_id, 64);

	return rc;
}

void store_quota_end_turn(struct session_store *store, const char *turn_id)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};

	if (!store || !turn_id || !*turn_id || orm_scope_begin(&store->db, &scope) != 0)
		return;

	struct norm_error error = {0};
	const struct norm_delete_query query = {
		.where = norm_eq(norm_col(&orm_fcgi_running_turns, 0), orm_string(turn_id)),
	};
	int rc = orm_status(&store->db,
			    norm_delete(scope.connection, &orm_fcgi_running_turns, &query,
					orm_scope_call(&scope), NULL, &error),
			    &error);

	(void)rc;
}

void store_turn_heartbeat(struct session_store *store, const char *turn_id)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};

	if (!store || !turn_id || !*turn_id || orm_scope_begin(&store->db, &scope) != 0)
		return;

	struct norm_error error = {0};
	const struct norm_assignment set[] = {
		{.column = norm_col(&orm_fcgi_running_turns, 4),
		 .value = norm_i64(fcgi_store_now())},
	};
	const struct norm_update_query query = {
		.set = set,
		.set_count = 1,
		.where = norm_eq(norm_col(&orm_fcgi_running_turns, 0), orm_string(turn_id)),
	};
	int rc = orm_status(&store->db,
			    norm_update(scope.connection, &orm_fcgi_running_turns, &query,
					orm_scope_call(&scope), NULL, &error),
			    &error);

	(void)rc;
}
