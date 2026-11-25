#include "persistence/orm/database.h"
#include "persistence/orm/internal.h"
#include "errors.h"
#include <nb/buf.h>
#include <nb/encoding.h>
#include <nb/file.h>
#include <nb/log.h>
#include <nb/thread.h>
#include <nb/utf8.h>
#include <nb/vec.h>
#include <stdlib.h>
#include <string.h>

struct orm_savepoint {
	char *name;
	bool owns_transaction;
};

struct orm_database {
	struct norm_db *pool;
	struct norm_conn *connection;
	nb_mutex *mutex;
	nb_vec savepoints;
	bool write_active;
};

static void orm_savepoints_clear(struct orm_database *database, size_t first)
{
	for (size_t index = first; index < database->savepoints.len; index++) {
		struct orm_savepoint *entry = nb_vec_at(&database->savepoints, index);

		free(entry->name);
	}

	database->savepoints.len = first;
}

static void orm_database_cleanup(struct orm_database **owner)
{
	struct orm_database *database = *owner;

	if (!database)
		return;

	if (database->connection)
		norm_conn_release(&database->connection, NULL);

	if (database->pool)
		norm_db_close(&database->pool, NULL, NULL);

	orm_savepoints_clear(database, 0);
	nb_vec_destroy(&database->savepoints);
	nb_mutex_free(database->mutex);
	free(database);
	*owner = NULL;
}

int orm_status(const struct db *db, nb_status status, const struct norm_error *error)
{
	if (status == NB_OK)
		return NB_OK;

	nb_log_write(db ? db->logger : NULL, NB_LOG_DEBUG, "ORM operation failed: %s (%s)",
		     error ? error->base.msg : norm_status_name(status), norm_status_name(status));
	if (status == NORM_ERR_CONSTRAINT || status == NORM_ERR_UNIQUE ||
	    status == NORM_ERR_FOREIGN_KEY || status == NORM_ERR_NOT_NULL ||
	    status == NORM_ERR_SCHEMA || status == NB_ERR_IO || status == NB_ERR_PARSE)
		return MORPH_ERR_DB;

	return status;
}

static nb_status orm_text_contains(void *context, size_t count, const struct norm_value *arguments,
				   struct norm_value *result, struct norm_error *error)
{
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf query NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_status rc;

	(void)context;
	(void)error;
	*result = norm_i64(0);
	if (count != 2)
		return NB_ERR_INVAL;

	if (arguments[0].type == NORM_TYPE_NULL || arguments[1].type == NORM_TYPE_NULL)
		return NB_OK;

	if (arguments[0].type != NORM_TYPE_TEXT || arguments[1].type != NORM_TYPE_TEXT)
		return NB_ERR_INVAL;

	rc = nb_utf8_casefold(arguments[0].as.text, &text, NULL);
	if (rc == NB_OK)
		rc = nb_utf8_casefold(arguments[1].as.text, &query, NULL);

	if (rc == NB_OK)
		*result = norm_i64(strstr(nb_buf_cstr(&text), nb_buf_cstr(&query)) != NULL);

	return rc;
}

static nb_status orm_connection_initialize(struct norm_conn *connection, void *context,
					   void **connection_context, const struct norm_call *call,
					   struct norm_error *error)
{
	const struct norm_sqlite_scalar scalar = {
		.name = NB_STR_LIT("morph_text_contains"),
		.arguments = 2,
		.deterministic = true,
		.innocuous = true,
		.invoke = orm_text_contains,
	};

	(void)context;
	(void)connection_context;
	return norm_sqlite_register_scalar(connection, &scalar, call, error);
}

static int orm_open(struct nb_log *logger, struct db *db, const char *path, bool readonly, bool wal)
{
	struct orm_database *database NB_CLEANUP(orm_database_cleanup) = NULL;
	nb_buf absolute NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf url NB_CLEANUP(nb_buf_cleanup) = {0};
	struct norm_error error = {0};
	struct norm_db_options options = {
		.pool_max = 1,
		.read_only = readonly,
		.logger = logger,
		.hooks = {.initialize = orm_connection_initialize},
	};
	int rc;

	if (!db || !path || !path[0])
		return NB_ERR_INVAL;

	memset(db, 0, sizeof(*db));
	db->logger = logger;
	rc = nb_str_copy_to(nb_str_from_cstr(path), db->path, sizeof(db->path));
	if (rc != NB_OK)
		return rc == NB_ERR_RANGE ? NB_ERR_NAME_TOO_LONG : rc;

	if (strcmp(path, ":memory:") == 0) {
		rc = nb_buf_append_cstr(&url, "sqlite:///:memory:") ? NB_OK : NB_ERR_NOMEM;
	} else {
		rc = nb_path_absolute(&absolute, path, NULL);
		if (rc == NB_OK && !nb_buf_append_cstr(&url, "sqlite:///"))
			rc = NB_ERR_NOMEM;

		if (rc == NB_OK) {
			nb_str value = nb_buf_str(&absolute);

			if (value.len && value.ptr[0] == '/') {
				value.ptr++;
				value.len--;
			}

			rc = nb_url_encode(value, false, &url, NULL);
		}

		options.sqlite_wal = wal && !readonly;
	}

	if (rc != NB_OK)
		return rc;

	database = calloc(1, sizeof(*database));
	if (!database)
		return NB_ERR_NOMEM;

	nb_vec_init(&database->savepoints, sizeof(struct orm_savepoint));
	database->mutex = nb_mutex_new(NB_MUTEX_RECURSIVE);
	if (!database->mutex)
		return NB_ERR_NOMEM;

	options.url = nb_buf_str(&url);
	rc = norm_db_open(&options, &database->pool, &error);
	if (rc == NB_OK)
		rc = norm_db_acquire(database->pool, NULL, &database->connection, &error);

	if (rc != NB_OK)
		return orm_status(db, rc, &error);

	db->storage = database;
	database = NULL;
	nb_log_write(logger, NB_LOG_DEBUG, "database opened through ORM: %s", path);
	return NB_OK;
}

int db_open(struct nb_log *logger, struct db *db, const char *path)
{
	return orm_open(logger, db, path, false, true);
}

int db_open_readonly(struct nb_log *logger, struct db *db, const char *path)
{
	return orm_open(logger, db, path, true, false);
}

int db_open_plain(struct nb_log *logger, struct db *db, const char *path)
{
	return orm_open(logger, db, path, false, false);
}

bool db_is_open(const struct db *db)
{
	return db && db->storage && db->storage->connection;
}

void db_close(struct db *db)
{
	if (!db || !db->storage)
		return;

	orm_database_cleanup(&db->storage);
}

int db_lock_acquire(struct db *db, struct db_lock *lock)
{
	int rc;

	if (!db_is_open(db) || !lock)
		return NB_ERR_INVAL;

	if (lock->storage)
		return NB_ERR_EXISTS;

	rc = nb_mutex_lock(db->storage->mutex);
	if (rc == NB_OK)
		lock->storage = db->storage;

	return rc;
}

void db_lock_cleanup(struct db_lock *lock)
{
	if (!lock || !lock->storage)
		return;

	nb_mutex_unlock(lock->storage->mutex);
	lock->storage = NULL;
}

int orm_scope_begin(const struct db *db, struct orm_scope *scope)
{
	int rc;

	if (!db_is_open(db) || !scope)
		return NB_ERR_INVAL;

	if (scope->database)
		return NB_ERR_EXISTS;

	rc = nb_mutex_lock(db->storage->mutex);
	if (rc == NB_OK) {
		scope->database = db;
		scope->connection = db->storage->connection;
	}

	return rc;
}

void orm_scope_cleanup(struct orm_scope *scope)
{
	if (!scope || !scope->database)
		return;

	nb_mutex_unlock(scope->database->storage->mutex);
	memset(scope, 0, sizeof(*scope));
}

void orm_rows_cleanup(struct norm_rows **rows)
{
	norm_rows_free(*rows);
	*rows = NULL;
}

int orm_select_rows(struct orm_scope *scope, const struct norm_model *model,
		    const struct norm_select_query *query, struct norm_rows **rows)
{
	struct norm_select_query selected = query ? *query : (struct norm_select_query){0};
	struct norm_error error = {0};

	if (!selected.result_options) {
		selected.max_rows = SIZE_MAX;
		selected.max_bytes = SIZE_MAX;
	}

	return orm_status(scope->database,
			  norm_select(scope->connection, model, &selected, NULL, rows, &error),
			  &error);
}

int orm_read(struct orm_scope *scope, const struct norm_query *query, struct norm_rows **rows)
{
	const struct norm_result_options unbounded = {
		.max_rows = SIZE_MAX, .max_bytes = SIZE_MAX, .max_row_bytes = SIZE_MAX};
	struct norm_query selected = *query;
	struct norm_error error = {0};

	if (!selected.result_options)
		selected.result_options = &unbounded;

	return orm_status(scope->database,
			  norm_query_select(scope->connection, &selected, NULL, rows, &error),
			  &error);
}

int orm_join_rows(struct orm_scope *scope, const struct norm_join_query *query,
		  struct norm_rows **rows)
{
	struct norm_join_query selected = *query;
	struct norm_error error = {0};

	if (!selected.select.result_options) {
		selected.select.max_rows = SIZE_MAX;
		selected.select.max_bytes = SIZE_MAX;
	}

	return orm_status(scope->database,
			  norm_join_select(scope->connection, &selected, NULL, rows, &error),
			  &error);
}

int orm_insert_row(struct orm_scope *scope, const struct norm_model *model, const void *row,
		   const struct norm_insert_options *options, struct norm_write_result *result)
{
	struct norm_error error = {0};

	return orm_status(scope->database,
			  norm_insert(scope->connection, model, row, options, NULL, result, &error),
			  &error);
}

int orm_update_rows(struct orm_scope *scope, const struct norm_model *model,
		    const struct norm_assignment *assignments, size_t count, struct norm_expr where,
		    struct norm_write_result *result)
{
	const struct norm_update_query query = {
		.set = assignments, .set_count = count, .where = where};
	struct norm_error error = {0};

	return orm_status(scope->database,
			  norm_update(scope->connection, model, &query, NULL, result, &error),
			  &error);
}

int orm_delete_rows(struct orm_scope *scope, const struct norm_model *model, struct norm_expr where,
		    struct norm_write_result *result)
{
	const struct norm_delete_query query = {.where = where};
	struct norm_error error = {0};

	return orm_status(scope->database,
			  norm_delete(scope->connection, model, &query, NULL, result, &error),
			  &error);
}

int orm_insert_from(struct orm_scope *scope, const struct norm_model *model, const size_t *fields,
		    size_t count, const struct norm_query *query,
		    const struct norm_conflict *conflict, struct norm_write_result *result)
{
	struct norm_error error = {0};

	return orm_status(scope->database,
			  norm_insert_select(scope->connection, model, fields, count, query,
					     conflict, NULL, result, &error),
			  &error);
}

int orm_transaction_begin(struct orm_scope *scope, enum norm_sqlite_tx_mode mode)
{
	const struct norm_tx_options options = {.sqlite_mode = mode};
	struct norm_error error = {0};

	return orm_status(scope->database, norm_tx_begin(scope->connection, &options, NULL, &error),
			  &error);
}

static int orm_transaction_end(struct orm_scope *scope, bool commit)
{
	struct norm_error error = {0};
	int rc = commit ? norm_tx_commit(scope->connection, NULL, &error)
			: norm_tx_rollback(scope->connection, NULL, &error);

	if (norm_tx_get_state(scope->connection) == NORM_TX_IDLE)
		orm_savepoints_clear(scope->database->storage, 0);

	return orm_status(scope->database, rc, &error);
}

int orm_transaction_commit(struct orm_scope *scope)
{
	return orm_transaction_end(scope, true);
}

int orm_transaction_rollback(struct orm_scope *scope)
{
	return orm_transaction_end(scope, false);
}

int orm_savepoint_begin(struct orm_scope *scope, const char *name)
{
	struct orm_database *database = scope->database->storage;
	struct orm_savepoint marker = {0};
	struct norm_error error = {0};
	bool created = false;
	int rc;

	if (!name || !*name)
		return NB_ERR_INVAL;

	marker.name = strdup(name);
	if (!marker.name)
		return NB_ERR_NOMEM;

	marker.owns_transaction = norm_tx_get_state(scope->connection) == NORM_TX_IDLE;
	rc = marker.owns_transaction ? orm_transaction_begin(scope, NORM_SQLITE_DEFERRED) : 0;
	if (rc == NB_OK)
		rc = orm_status(scope->database,
				norm_savepoint_create(scope->connection, nb_str_from_cstr(name),
						      NULL, &error),
				&error);

	created = rc == NB_OK;
	if (created && !nb_vec_push_copy(&database->savepoints, &marker))
		rc = NB_ERR_NOMEM;

	if (rc != NB_OK) {
		if (marker.owns_transaction) {
			orm_transaction_rollback(scope);
		} else if (created) {
			norm_savepoint_rollback(scope->connection, nb_str_from_cstr(name), NULL,
						NULL);
			norm_savepoint_release(scope->connection, nb_str_from_cstr(name), NULL,
					       NULL);
		}

		free(marker.name);
	}

	return rc;
}

static int orm_savepoint_end(struct orm_scope *scope, const char *name, bool rollback)
{
	struct orm_database *database = scope->database->storage;
	struct norm_error error = {0};
	size_t marker = database->savepoints.len;
	bool owns_transaction;
	int rc = NB_OK;

	while (marker) {
		const struct orm_savepoint *entry = nb_vec_at(&database->savepoints, marker - 1);

		if (strcmp(entry->name, name) == 0)
			break;

		marker--;
	}

	if (!marker)
		return NB_ERR_STATE;

	marker--;
	owns_transaction = ((struct orm_savepoint *)nb_vec_at(&database->savepoints, marker))
				   ->owns_transaction;
	if (rollback && owns_transaction)
		return orm_transaction_rollback(scope);

	if (rollback)
		rc = orm_status(scope->database,
				norm_savepoint_rollback(scope->connection, nb_str_from_cstr(name),
							NULL, &error),
				&error);

	if (rc == NB_OK)
		rc = orm_status(scope->database,
				norm_savepoint_release(scope->connection, nb_str_from_cstr(name),
						       NULL, &error),
				&error);

	if (owns_transaction) {
		if (rc == NB_OK)
			rc = orm_transaction_commit(scope);

		if (rc != NB_OK && norm_tx_get_state(scope->connection) != NORM_TX_IDLE)
			orm_transaction_rollback(scope);
	} else if (rc == NB_OK) {
		orm_savepoints_clear(database, marker);
	}

	return rc;
}

int orm_savepoint_abort(struct orm_scope *scope, const char *name)
{
	return orm_savepoint_end(scope, name, true);
}

int orm_savepoint_finish(struct orm_scope *scope, const char *name, int status)
{
	int rc = orm_savepoint_end(scope, name, status != NB_OK);

	return status ? status : rc;
}

int orm_write_begin(struct db *db, enum norm_sqlite_tx_mode mode)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);

	if (rc != NB_OK)
		return rc;

	if (db->storage->write_active)
		return NB_ERR_STATE;

	rc = orm_transaction_begin(&scope, mode);
	if (rc == NB_OK) {
		db->storage->write_active = true;
		/* Transfer this recursive lock level to the transaction owner. */
		scope.database = NULL;
		scope.connection = NULL;
	}

	return rc;
}

int orm_write_finish(struct db *db, int status)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);

	if (rc != NB_OK)
		return status ? status : rc;

	if (!db->storage->write_active)
		return status ? status : NB_ERR_STATE;

	rc = status ? orm_transaction_rollback(&scope) : orm_transaction_commit(&scope);
	if (!status && rc != NB_OK)
		orm_transaction_rollback(&scope);

	db->storage->write_active = false;
	/* Release the begin-owned level; the local scope releases its own level. */
	nb_mutex_unlock(db->storage->mutex);
	return status ? status : rc;
}
