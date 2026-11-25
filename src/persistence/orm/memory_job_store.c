#include "memory_job_store.h"
#include "memory_models.h"
#include "records.h"
#include "errors.h"
#include <limits.h>
#include <time.h>

void memory_job_records_cleanup(struct memory_job_records *records)
{
	if (!records)
		return;

	for (size_t i = 0; i < records->rows.len; i++) {
		struct memory_job_record *row = nb_vec_at(&records->rows, i);
		free(row->payload_json);
	}

	nb_vec_cleanup(&records->rows);
	memset(records, 0, sizeof(*records));
}

static void memory_job_records_move(struct memory_job_records *destination,
				    struct memory_job_records *source)
{
	*destination = *source;
	memset(source, 0, sizeof(*source));
}

static int read_records(struct db *db, const struct norm_select_query *query,
			struct memory_job_records *out)
{
	struct memory_job_records value NB_CLEANUP(memory_job_records_cleanup) = {0};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!out)
		return NB_ERR_INVAL;

	if (out->rows.data)
		return NB_ERR_EXISTS;

	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_select_rows(&scope, &job_model, query, &rows);

	if (rc != 0)
		return rc;

	nb_vec_init(&value.rows, sizeof(struct memory_job_record));
	for (size_t i = 0; i < norm_rows_count(rows); i++) {
		const struct memory_job_row *source = norm_rows_at(rows, i);
		struct memory_job_record *row = nb_vec_push(&value.rows);

		if (!row)
			return NB_ERR_NOMEM;

		memset(row, 0, sizeof(*row));
		row->id = source->id;
		row->session_id = source->session_id;
		row->memory_version = source->generation;
		row->worker_pid = source->worker_pid;
		row->attempts = source->attempts;
		row->error_code = source->error_code;
		if (nb_str_copy_to(source->state, row->state, sizeof(row->state)) != NB_OK ||
		    nb_str_copy_to(source->stage, row->stage, sizeof(row->stage)) != NB_OK)
			return NB_ERR_RANGE;

		if (norm_rows_has_field(rows, JOB_PAYLOAD) && !source->payload_missing) {
			row->payload_json = orm_text_copy(source->payload);
			if (!row->payload_json)
				return NB_ERR_NOMEM;
		}
	}

	memory_job_records_move(out, &value);
	return 0;
}

int memory_job_store_recoverable(struct db *db, struct memory_job_records *out)
{
	const struct norm_value active[] = {orm_string("queued"), orm_string("running")};
	const struct norm_expr failed[] = {
		norm_eq(norm_col(&job_model, JOB_STATE), orm_string("failed")),
		norm_lt(norm_col(&job_model, JOB_ATTEMPTS), norm_i32(3)),
		norm_lt(norm_col(&job_model, JOB_UPDATED_AT), norm_i64((int64_t)time(NULL) - 30)),
	};
	const struct norm_expr choices[] = {
		norm_in(norm_col(&job_model, JOB_STATE), active, NB_ARRAY_SIZE(active)),
		norm_and(failed, NB_ARRAY_SIZE(failed)),
	};
	const struct norm_order order = {.column = norm_col(&job_model, JOB_ID),
					 .direction = NORM_ASC};
	const struct norm_select_query query = {.where = norm_or(choices, NB_ARRAY_SIZE(choices)),
						.order = &order,
						.order_count = 1};

	return read_records(db, &query, out);
}

int memory_job_store_recent(struct db *db, int64_t session_id, int limit,
			    struct memory_job_records *out)
{
	if (session_id < 0 || limit <= 0)
		return NB_ERR_INVAL;

	const size_t fields[] = {JOB_ID,	 JOB_SESSION_ID, JOB_GENERATION, JOB_STATE,
				 JOB_WORKER_PID, JOB_ATTEMPTS,	 JOB_ERROR_CODE, JOB_STAGE};
	const struct norm_order order = {.column = norm_col(&job_model, JOB_ID),
					 .direction = NORM_DESC};
	const struct norm_select_query query = {
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.where = norm_eq(norm_col(&job_model, JOB_SESSION_ID), norm_i64(session_id)),
		.order = &order,
		.order_count = 1,
		.has_limit = true,
		.limit = (uint64_t)limit,
	};

	return read_records(db, &query, out);
}

struct job_revision {
	struct norm_table_ref scope;
	struct norm_query_field field;
	struct norm_query query;
	struct norm_term arguments[2];
	struct norm_term value;
};

static void job_revision_init(struct job_revision *revision, int64_t session_id, bool correlated)
{
	memset(revision, 0, sizeof(*revision));
	revision->scope = norm_table(&memory_scope_model, NB_STR_LIT("scope"));
	revision->field.value =
		norm_term_column(norm_table_col(&revision->scope, SCOPE_CONTENT_REVISION));
	revision->query.from = &revision->scope;
	revision->query.projection = &orm_integer_model;
	revision->query.fields = &revision->field;
	revision->query.field_count = 1;
	revision->query.where =
		correlated ? norm_columns_eq(norm_table_col(&revision->scope, SCOPE_SESSION_ID),
					     norm_col(&job_model, JOB_SESSION_ID))
			   : norm_eq(norm_table_col(&revision->scope, SCOPE_SESSION_ID),
				     norm_i64(session_id));
	revision->arguments[0] = norm_term_scalar(&revision->query);
	revision->arguments[1] = norm_term_value(norm_i64(0));
	revision->value = norm_term_call(NORM_FUNCTION_COALESCE, revision->arguments,
					 NB_ARRAY_SIZE(revision->arguments));
}

int memory_job_store_add(struct db *db, int64_t session_id, int64_t memory_version,
			 const char *payload_json, int64_t *id)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_write_result result = {0};

	if (session_id <= 0 || !payload_json || !id)
		return NB_ERR_INVAL;

	*id = 0;
	struct job_revision revision = {0};

	job_revision_init(&revision, session_id, false);
	const size_t targets[] = {JOB_SESSION_ID, JOB_GENERATION, JOB_PAYLOAD,
				  JOB_CONTENT_REVISION};
	const struct norm_query_field fields[] = {
		{.value = norm_term_value(norm_i64(session_id)), .into = JOB_SESSION_ID},
		{.value = norm_term_value(norm_i64(memory_version)), .into = JOB_GENERATION},
		{.value = norm_term_value(orm_string(payload_json)), .into = JOB_PAYLOAD},
		{.value = revision.value, .into = JOB_CONTENT_REVISION},
	};
	const struct norm_query query = {
		.projection = &job_model, .fields = fields, .field_count = NB_ARRAY_SIZE(fields)};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_insert_from(&scope, &job_model, targets, NB_ARRAY_SIZE(targets), &query,
				     NULL, &result);

	if (rc == 0 && result.has_insert_id)
		*id = result.insert_id;

	return rc;
}

static int change_job(struct db *db, const struct norm_assignment *assignments, size_t count,
		      const struct norm_expr *conditions, size_t condition_count)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_write_result result = {0};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_update_rows(&scope, &job_model, assignments, count,
				     norm_and(conditions, condition_count), &result);

	if (rc != 0)
		return rc;

	return result.affected_rows > INT_MAX ? NB_ERR_RANGE : (int)result.affected_rows;
}

int memory_job_store_claim(struct db *db, int64_t id, int64_t worker_pid)
{
	if (id <= 0)
		return NB_ERR_INVAL;

	const struct norm_term attempts = norm_term_column(norm_col(&job_model, JOB_ATTEMPTS));
	const struct norm_term one = norm_term_value(norm_i32(1));
	const struct norm_term next = norm_term_binary(NORM_BINARY_ADD, &attempts, &one);
	const struct norm_assignment assignments[] = {
		{.column = norm_col(&job_model, JOB_STATE), .value = orm_string("running")},
		{.column = norm_col(&job_model, JOB_ATTEMPTS), .expression = &next},
		{.column = norm_col(&job_model, JOB_WORKER_PID), .value = norm_i64(worker_pid)},
		{.column = norm_col(&job_model, JOB_UPDATED_AT),
		 .value = norm_i64((int64_t)time(NULL))},
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&job_model, JOB_ID), norm_i64(id)),
		norm_eq(norm_col(&job_model, JOB_STATE), orm_string("queued"))};

	return change_job(db, assignments, NB_ARRAY_SIZE(assignments), conditions,
			  NB_ARRAY_SIZE(conditions));
}

int memory_job_store_requeue(struct db *db, int64_t id, const char *expected_state)
{
	if (id <= 0 || !expected_state)
		return NB_ERR_INVAL;

	const struct norm_assignment assignment = {.column = norm_col(&job_model, JOB_STATE),
						   .value = orm_string("queued")};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&job_model, JOB_ID), norm_i64(id)),
		norm_eq(norm_col(&job_model, JOB_STATE), orm_string(expected_state))};

	return change_job(db, &assignment, 1, conditions, NB_ARRAY_SIZE(conditions));
}

int memory_job_store_complete(struct db *db, int64_t id, int error)
{
	if (id <= 0)
		return NB_ERR_INVAL;

	const char *state = error == NB_ERR_STALE ? "cancelled" : error ? "failed" : "completed";
	struct job_revision revision = {0};

	job_revision_init(&revision, 0, true);
	const struct norm_assignment assignments[] = {
		{.column = norm_col(&job_model, JOB_STATE), .value = orm_string(state)},
		{.column = norm_col(&job_model, JOB_ERROR_CODE), .value = norm_i32(error)},
		{.column = norm_col(&job_model, JOB_CONTENT_REVISION),
		 .expression = &revision.value},
		{.column = norm_col(&job_model, JOB_UPDATED_AT),
		 .value = norm_i64((int64_t)time(NULL))},
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&job_model, JOB_ID), norm_i64(id)),
		norm_eq(norm_col(&job_model, JOB_STATE), orm_string("running"))};
	int rc = change_job(db, assignments, NB_ARRAY_SIZE(assignments), conditions,
			    NB_ARRAY_SIZE(conditions));

	return rc < 0 ? rc : 0;
}

int memory_job_store_stage(struct db *db, int64_t id, const char *stage)
{
	if (id <= 0 || !stage || (strcmp(stage, "extract") && strcmp(stage, "organize")))
		return NB_ERR_INVAL;

	struct job_revision revision = {0};

	job_revision_init(&revision, 0, true);
	const struct norm_value active[] = {orm_string("queued"), orm_string("running")};
	const struct norm_assignment assignments[] = {
		{.column = norm_col(&job_model, JOB_STAGE), .value = orm_string(stage)},
		{.column = norm_col(&job_model, JOB_CONTENT_REVISION),
		 .expression = &revision.value},
		{.column = norm_col(&job_model, JOB_UPDATED_AT),
		 .value = norm_i64((int64_t)time(NULL))},
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&job_model, JOB_ID), norm_i64(id)),
		norm_in(norm_col(&job_model, JOB_STATE), active, NB_ARRAY_SIZE(active))};
	int rc = change_job(db, assignments, NB_ARRAY_SIZE(assignments), conditions,
			    NB_ARRAY_SIZE(conditions));

	return rc < 0 ? rc : 0;
}

int memory_job_store_retry_session(struct db *db, int64_t session_id)
{
	if (session_id <= 0)
		return NB_ERR_INVAL;

	const struct norm_assignment assignments[] = {
		{.column = norm_col(&job_model, JOB_STATE), .value = orm_string("queued")},
		{.column = norm_col(&job_model, JOB_ATTEMPTS), .value = norm_i32(0)},
		{.column = norm_col(&job_model, JOB_ERROR_CODE), .value = norm_i32(0)},
		{.column = norm_col(&job_model, JOB_WORKER_PID), .value = norm_i64(0)},
	};
	const struct norm_expr conditions[] = {
		norm_eq(norm_col(&job_model, JOB_SESSION_ID), norm_i64(session_id)),
		norm_eq(norm_col(&job_model, JOB_STATE), orm_string("failed"))};

	return change_job(db, assignments, NB_ARRAY_SIZE(assignments), conditions,
			  NB_ARRAY_SIZE(conditions));
}
