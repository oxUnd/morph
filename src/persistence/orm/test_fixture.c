#include "test_fixture.h"
#include "persistence/orm/records.h"
#include <nb/buf.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int fixture_sql_query(struct orm_scope *scope, const struct norm_model *projection,
			     const char *sql, const struct norm_value *parameters, size_t count,
			     struct norm_rows **rows)
{
	const struct norm_result_options options = {
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
		.max_row_bytes = SIZE_MAX,
	};
	struct norm_error error = {0};

	if (!scope || !scope->connection || !sql || !projection || !rows)
		return NB_ERR_INVAL;

	return orm_status(scope->database,
			  norm_sql_query_with_options(scope->connection, projection,
						      nb_str_from_cstr(sql), parameters, count,
						      &options, NULL, rows, &error),
			  &error);
}

static int fixture_sql_script(struct orm_scope *scope, const char *sql)
{
	struct norm_error error = {0};

	if (!scope || !scope->connection || !sql)
		return NB_ERR_INVAL;

	return orm_status(
		scope->database,
		norm_sqlite_exec_script(scope->connection, nb_str_from_cstr(sql), NULL, &error),
		&error);
}

struct fixture_integer {
	int64_t value;
	bool missing;
};

static const struct norm_field fixture_integer_fields[] = {
	NORM_NULLABLE_FIELD(struct fixture_integer, value, missing, NORM_TYPE_I64, 0),
};
static const struct norm_model fixture_integer_model =
	NORM_MODEL(struct fixture_integer, "", fixture_integer_fields);

static int fixture_query(struct orm_scope *scope, const struct norm_model *model, const char *query,
			 bool count, struct norm_rows **rows)
{
	nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};
	size_t length;

	if (!query)
		return NB_ERR_INVAL;

	length = strlen(query);
	while (length && (isspace((unsigned char)query[length - 1]) || query[length - 1] == ';'))
		length--;

	if (!length)
		return NB_ERR_INVAL;

	nb_buf_append_cstr(&sql, count ? "WITH __fixture AS (" : "WITH __fixture(value) AS (");
	nb_buf_append(&sql, query, length);
	nb_buf_append_cstr(&sql, count ? ") SELECT COUNT(*) AS value FROM __fixture"
				       : ") SELECT value FROM __fixture");
	if (!nb_buf_ok(&sql))
		return NB_ERR_NOMEM;

	return fixture_sql_query(scope, model, nb_buf_cstr(&sql), NULL, 0, rows);
}

int orm_fixture_execute(struct db *db, const char *script)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	int rc = orm_scope_begin(db, &scope);

	if (rc != NB_OK || !script)
		return rc != NB_OK ? rc : NB_ERR_INVAL;

	/* Fixtures use the public maintenance API for these lease settings. The
	 * remaining script is parsed and executed by the ORM driver. */
	for (;;) {
		while (isspace((unsigned char)*script))
			script++;

		if (strncmp(script, "PRAGMA journal_mode=WAL;", 24) == 0) {
			rc = norm_sqlite_set_journal_mode(scope.connection, NORM_SQLITE_JOURNAL_WAL,
							  NULL, &error);
			script += 24;
		} else if (strncmp(script, "PRAGMA wal_autocheckpoint=0;", 28) == 0) {
			const struct norm_sqlite_config config = {
				.fields = NORM_SQLITE_CONFIG_WAL_AUTOCHECKPOINT,
				.wal_autocheckpoint_pages = 0,
			};

			rc = norm_sqlite_configure(scope.connection, &config, NULL, &error);
			script += 28;
		} else {
			break;
		}

		if (rc != NB_OK)
			return orm_status(db, rc, &error);
	}

	return fixture_sql_script(&scope, script);
}

int orm_fixture_integer(struct db *db, const char *query, int64_t *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	int rc;

	if (!out)
		return NB_ERR_INVAL;

	rc = orm_scope_begin(db, &scope);
	if (rc == NB_OK)
		rc = fixture_query(&scope, &fixture_integer_model, query, false, &rows);

	if (rc == NB_OK) {
		const struct fixture_integer *row = norm_rows_at(rows, 0);

		if (!row)
			return NB_ERR_NO_DATA;

		*out = row->missing ? 0 : row->value;
	}

	return rc;
}

int orm_fixture_text_copy(struct db *db, const char *query, char **out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	int rc;

	if (!out)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	rc = orm_scope_begin(db, &scope);
	if (rc == NB_OK)
		rc = fixture_query(&scope, &orm_text_model, query, false, &rows);

	if (rc == NB_OK) {
		const struct orm_text *row = norm_rows_at(rows, 0);

		if (!row)
			return NB_ERR_NO_DATA;

		if (!row->missing) {
			*out = orm_text_copy(row->value);
			if (!*out)
				return NB_ERR_NOMEM;
		}
	}

	return rc;
}

int orm_fixture_row_count(struct db *db, const char *query, size_t *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	int rc;

	if (!out)
		return NB_ERR_INVAL;

	rc = orm_scope_begin(db, &scope);
	if (rc == NB_OK)
		rc = fixture_query(&scope, &orm_integer_model, query, true, &rows);

	if (rc == NB_OK) {
		const struct orm_integer *row = norm_rows_at(rows, 0);

		if (!row || row->value < 0)
			return NB_ERR_NO_DATA;

		*out = (size_t)row->value;
	}

	return rc;
}

bool orm_fixture_has_column(struct db *db, const char *schema, const char *table,
			    const char *column)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	bool has = false;

	return orm_scope_begin(db, &scope) == NB_OK &&
	       orm_table_has_column(&scope, schema, table, column, &has) == NB_OK && has;
}

bool orm_fixture_transaction_idle(struct db *db)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};

	return orm_scope_begin(db, &scope) == NB_OK &&
	       norm_tx_get_state(scope.connection) == NORM_TX_IDLE;
}

size_t orm_fixture_statement_count(struct db *db)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	size_t count = SIZE_MAX;

	if (orm_scope_begin(db, &scope) == NB_OK)
		norm_sqlite_testing_statement_count(scope.connection, &count, &error);

	return count;
}

int orm_fixture_commit_hook(struct db *db, int (*callback)(void *), void *context)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	int rc = orm_scope_begin(db, &scope);

	if (rc == NB_OK)
		rc = orm_status(db,
				norm_sqlite_testing_commit_hook(scope.connection, callback, context,
								&error),
				&error);

	return rc;
}

int orm_fixture_progress_hook(struct db *db, int instructions, int (*callback)(void *),
			      void *context)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	int rc = orm_scope_begin(db, &scope);

	if (rc == NB_OK)
		rc = orm_status(db,
				norm_sqlite_testing_progress_hook(scope.connection, instructions,
								  callback, context, &error),
				&error);

	return rc;
}

int orm_fixture_trace_hook(struct db *db, void (*callback)(void *, const char *), void *context)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	int rc = orm_scope_begin(db, &scope);

	if (rc == NB_OK)
		rc = orm_status(
			db,
			norm_sqlite_testing_trace_hook(scope.connection, callback, context, &error),
			&error);

	return rc;
}

static nb_status fixture_scalar_error(void *context, size_t count,
				      const struct norm_value *arguments, struct norm_value *result,
				      struct norm_error *error)
{
	(void)count;
	(void)arguments;
	(void)result;
	(void)error;
	(void)context;
	return NB_ERR_IO;
}

int orm_fixture_scalar_error(struct db *db, const char *name, const char *message)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_sqlite_scalar scalar = {
		.name = nb_str_from_cstr(name),
		.arguments = 0,
		.invoke = fixture_scalar_error,
		.context = (void *)message,
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == NB_OK)
		rc = orm_status(
			db, norm_sqlite_register_scalar(scope.connection, &scalar, NULL, &error),
			&error);

	return rc;
}

int orm_fixture_scalar_hook(struct db *db, const char *name, int (*callback)(void *), void *context)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	int rc = orm_scope_begin(db, &scope);

	if (rc == NB_OK)
		rc = orm_status(db,
				norm_sqlite_testing_scalar_hook(scope.connection,
								nb_str_from_cstr(name), callback,
								context, &error),
				&error);

	return rc;
}
