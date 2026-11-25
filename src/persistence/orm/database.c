#include "persistence/orm/database.h"
#include "persistence/orm/internal.h"
#include "errors.h"
#include <nb/buf.h>
#include <nb/encoding.h>
#include <nb/file.h>
#include <nb/log.h>
#include <nb/thread.h>
#include <nb/time.h>
#include <stdio.h>
#include <nb/utf8.h>
#include <nb/vec.h>
#include <stdlib.h>
#include <string.h>

struct orm_savepoint {
	char *name;
	bool owns_transaction;
};

struct db_connection_config {
	enum db_backend backend;
	char *path;
	char *url;
	char *ca_file;
	char *cert_file;
	char *key_file;
	enum db_tls_mode tls_mode;
	int connect_timeout_ms;
	int query_timeout_ms;
	bool readonly;
	bool wal;
};

struct orm_database {
	struct norm_db *pool;
	struct norm_conn *connection;
	nb_mutex *mutex;
	nb_vec savepoints;
	bool write_active;
	const struct nb_cancel *cancel;
	struct db_connection_config *config;
	char *checkpoint_token;
	bool checkpoint_ready;
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
	db_connection_config_cleanup(&database->config);
	free(database->checkpoint_token);
	free(database);
	*owner = NULL;
}

int orm_status(const struct db *db, nb_status status, const struct norm_error *error)
{
	if (status == NB_OK)
		return NB_OK;

	nb_log_write(db ? db->logger : NULL, NB_LOG_DEBUG, "ORM operation failed: %s (%s)",
		     error ? error->base.msg : norm_status_name(status), norm_status_name(status));
	if (status == NORM_ERR_OUTCOME_UNKNOWN)
		return MORPH_ERR_DB_OUTCOME_UNKNOWN;

	if (status == NORM_ERR_SERIALIZATION || status == NORM_ERR_DEADLOCK)
		return NB_ERR_BUSY;

	if (status == NORM_ERR_EXTENSION)
		return NB_ERR_UNSUPPORTED;

	if (status == NORM_ERR_CONSTRAINT || status == NORM_ERR_UNIQUE ||
	    status == NORM_ERR_FOREIGN_KEY || status == NORM_ERR_NOT_NULL ||
	    status == NORM_ERR_SCHEMA || status == NORM_ERR_MULTIPLE_ROWS ||
	    status == NB_ERR_IO || status == NB_ERR_PARSE)
		return MORPH_ERR_DB;

	return status;
}

static nb_status orm_connection_configure(struct norm_conn *connection, void *context,
					  void *connection_context, const struct norm_call *call,
					  struct norm_error *error)
{
	const struct db_connection_config *config = context;
	char timeout[BUFSIZ];

	(void)connection_context;
	if (norm_conn_driver(connection) != NORM_DRIVER_POSTGRESQL)
		return NB_OK;

	int count = snprintf(timeout, sizeof(timeout), "%d", config->query_timeout_ms);

	if (count < 0 || (size_t)count >= sizeof(timeout))
		return NB_ERR_RANGE;

	return norm_session_set(connection, NB_STR_LIT("statement_timeout"),
				nb_str_from_cstr(timeout), call, error);
}

void db_connection_config_cleanup(struct db_connection_config **owner)
{
	struct db_connection_config *config = owner ? *owner : NULL;

	if (!config)
		return;

	free(config->path);
	free(config->url);
	free(config->ca_file);
	free(config->cert_file);
	free(config->key_file);
	free(config);
	*owner = NULL;
}

static int connection_config_clone(const struct db_connection_config *source,
				   struct db_connection_config **out)
{
	struct db_connection_config *copy NB_CLEANUP(db_connection_config_cleanup) = NULL;

	if (!source || !out)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	copy = calloc(1, sizeof(*copy));
	if (!copy)
		return NB_ERR_NOMEM;

	copy->backend = source->backend;
	copy->tls_mode = source->tls_mode;
	copy->connect_timeout_ms = source->connect_timeout_ms;
	copy->query_timeout_ms = source->query_timeout_ms;
	copy->readonly = source->readonly;
	copy->wal = source->wal;
	copy->path = strdup(source->path ? source->path : "");
	copy->url = strdup(source->url ? source->url : "");
	copy->ca_file = strdup(source->ca_file ? source->ca_file : "");
	copy->cert_file = strdup(source->cert_file ? source->cert_file : "");
	copy->key_file = strdup(source->key_file ? source->key_file : "");
	if (!copy->path || !copy->url || !copy->ca_file || !copy->cert_file || !copy->key_file)
		return NB_ERR_NOMEM;

	*out = copy;
	copy = NULL;
	return NB_OK;
}

int db_connection_config_copy(const struct db *db, struct db_connection_config **out)
{
	if (!db_is_open(db))
		return NB_ERR_INVAL;

	return connection_config_clone(db->storage->config, out);
}

enum db_backend db_backend(const struct db *db)
{
	return db && db->storage && db->storage->config
		       ? db->storage->config->backend : DB_BACKEND_SQLITE;
}

bool db_is_memory(const struct db *db)
{
	return db && db_backend(db) == DB_BACKEND_SQLITE && strcmp(db->path, ":memory:") == 0;
}

bool db_connection_config_matches(const struct db_connection_config *config,
				  const struct db *db)
{
	const struct db_connection_config *other = db_is_open(db) ? db->storage->config : NULL;

	return config && other && config->backend == other->backend &&
	       strcmp(config->url, other->url) == 0 && strcmp(config->path, other->path) == 0;
}

struct orm_write_guard_row {
	int32_t id;
	int64_t epoch;
};
static const struct norm_field orm_write_guard_fields[] = {
	NORM_FIELD(struct orm_write_guard_row, id, NORM_TYPE_I32, NORM_FIELD_PRIMARY_KEY),
	NORM_FIELD(struct orm_write_guard_row, epoch, NORM_TYPE_I64, 0),
};
static const struct norm_model orm_write_guard_model =
	NORM_MODEL(struct orm_write_guard_row, "morph_write_guard", orm_write_guard_fields);

static int orm_write_guard_initialize(struct norm_conn *connection, const struct norm_call *call,
				      struct norm_error *error)
{
	const struct norm_schema_options create = {.if_not_exists = true};
	const struct orm_write_guard_row row = {.id = 1};
	const struct norm_conflict conflict = {.action = NORM_CONFLICT_NOTHING};
	const struct norm_insert_options insert = {.conflict = &conflict};
	int rc = norm_schema_create(connection, &orm_write_guard_model, &create, call, error);

	if (rc == NORM_ERR_UNIQUE)
		rc = norm_schema_create(connection, &orm_write_guard_model, &create, call, error);

	if (rc == NB_OK)
		rc = norm_insert(connection, &orm_write_guard_model, &row, &insert, call, NULL,
				 error);

	return rc;
}

int db_open_connection(struct nb_log *logger, struct db *db,
		       const struct db_connection_config *config)
{
	struct orm_database *database NB_CLEANUP(orm_database_cleanup) = NULL;
	struct norm_error error = {0};
	struct norm_db_options options = {
		.pool_max = 1,
		.logger = logger,
		.hooks = {.configure = orm_connection_configure},
	};
	int rc;

	if (!db || !config || !config->url || !config->url[0])
		return NB_ERR_INVAL;

	memset(db, 0, sizeof(*db));
	db->logger = logger;
	database = calloc(1, sizeof(*database));
	if (!database)
		return NB_ERR_NOMEM;

	nb_vec_init(&database->savepoints, sizeof(struct orm_savepoint));
	database->mutex = nb_mutex_new(NB_MUTEX_RECURSIVE);
	if (!database->mutex)
		return NB_ERR_NOMEM;

	rc = connection_config_clone(config, &database->config);
	if (rc != NB_OK)
		return rc;

	if (config->backend == DB_BACKEND_SQLITE) {
		rc = nb_str_copy_to(nb_str_from_cstr(config->path), db->path, sizeof(db->path));
		if (rc != NB_OK)
			return rc == NB_ERR_RANGE ? NB_ERR_NAME_TOO_LONG : rc;

		options.read_only = config->readonly;
		options.sqlite_wal = config->wal && !config->readonly &&
				     strcmp(config->path, ":memory:") != 0;
	}

	options.hooks.context = database->config;
	options.url = nb_str_from_cstr(config->url);
	options.tls.mode = (enum norm_tls_mode)config->tls_mode;
	options.tls.ca_file = nb_str_from_cstr(config->ca_file);
	options.tls.cert_file = nb_str_from_cstr(config->cert_file);
	options.tls.key_file = nb_str_from_cstr(config->key_file);
	options.connect_timeout_ms = config->connect_timeout_ms;
	rc = norm_db_open(&options, &database->pool, &error);
	if (rc == NB_OK)
		rc = norm_db_acquire(database->pool, NULL, &database->connection, &error);

	if (rc == NB_OK && config->backend == DB_BACKEND_POSTGRESQL) {
		const struct norm_call call = {
			.deadline_ns = nb_time_deadline_after_ns(
				(int64_t)config->query_timeout_ms * INT64_C(1000000))};

		rc = orm_write_guard_initialize(database->connection, &call, &error);
	}

	if (rc != NB_OK)
		return orm_status(db, rc, &error);

	db->storage = database;
	database = NULL;
	nb_log_write(logger, NB_LOG_DEBUG, "database opened through ORM: %s",
		     config->backend == DB_BACKEND_SQLITE ? config->path : "postgresql");
	return NB_OK;
}

static int orm_open_options(struct nb_log *logger, struct db *db,
			    const struct db_options *options, bool readonly, bool wal)
{
	nb_buf absolute NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf url NB_CLEANUP(nb_buf_cleanup) = {0};
	struct db_connection_config config = {0};
	int rc;

	if (!options || options->backend < DB_BACKEND_SQLITE ||
	    options->backend > DB_BACKEND_POSTGRESQL || options->tls_mode < DB_TLS_DEFAULT ||
	    options->tls_mode > DB_TLS_VERIFY_FULL || options->connect_timeout_ms < 0 ||
	    options->query_timeout_ms < 0)
		return NB_ERR_INVAL;

	config.backend = options->backend;
	config.tls_mode = options->tls_mode;
	config.connect_timeout_ms = options->connect_timeout_ms;
	config.query_timeout_ms = options->query_timeout_ms > 0 ? options->query_timeout_ms : 30000;
	config.ca_file = (char *)options->ca_file;
	config.cert_file = (char *)options->cert_file;
	config.key_file = (char *)options->key_file;
	config.readonly = readonly;
	config.wal = wal;
	if (options->backend == DB_BACKEND_POSTGRESQL) {
		const char *value = options->url_env && options->url_env[0]
				    ? getenv(options->url_env) : NULL;

		if (!value || strncmp(value, "postgresql://", 13) != 0)
			return NB_ERR_INVAL;

		config.url = (char *)value;
		return db_open_connection(logger, db, &config);
	}

	if (!options->path || !options->path[0])
		return NB_ERR_INVAL;

	config.path = (char *)options->path;
	if (strcmp(options->path, ":memory:") == 0) {
		rc = nb_buf_append_cstr(&url, "sqlite:///:memory:") ? NB_OK : NB_ERR_NOMEM;
	} else {
		rc = nb_path_absolute(&absolute, options->path, NULL);
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
	}

	if (rc != NB_OK)
		return rc;

	config.url = (char *)nb_buf_cstr(&url);
	return db_open_connection(logger, db, &config);
}

int db_open_options(struct nb_log *logger, struct db *db, const struct db_options *options)
{
	return orm_open_options(logger, db, options, false, true);
}

int db_reopen(struct nb_log *logger, struct db *out, const struct db *source)
{
	if (!db_is_open(source) || out == source)
		return NB_ERR_INVAL;

	return db_open_connection(logger, out, source->storage->config);
}

int db_open(struct nb_log *logger, struct db *db, const char *path)
{
	const struct db_options options = {.path = path};

	return db_open_options(logger, db, &options);
}

int db_open_readonly(struct nb_log *logger, struct db *db, const char *path)
{
	const struct db_options options = {.path = path};

	return orm_open_options(logger, db, &options, true, false);
}

int db_open_plain(struct nb_log *logger, struct db *db, const char *path)
{
	const struct db_options options = {.path = path};

	return orm_open_options(logger, db, &options, false, false);
}

bool orm_checkpoint_ready(const struct db *db)
{
	return db_is_open(db) && db->storage->checkpoint_ready;
}

void orm_checkpoint_set_ready(const struct db *db, bool ready)
{
	if (db_is_open(db))
		db->storage->checkpoint_ready = ready;
}

const char *orm_checkpoint_token(const struct db *db)
{
	return db_is_open(db) ? db->storage->checkpoint_token : NULL;
}

int orm_checkpoint_set_token(const struct db *db, const char *token)
{
	char *copy NB_CLEANUP(nb_charp_cleanup) = NULL;

	if (!db_is_open(db))
		return NB_ERR_INVAL;

	copy = token ? strdup(token) : NULL;
	if (token && !copy)
		return NB_ERR_NOMEM;

	free(db->storage->checkpoint_token);
	db->storage->checkpoint_token = copy;
	copy = NULL;
	return NB_OK;
}

bool db_is_open(const struct db *db)
{
	return db && db->storage && db->storage->connection;
}

int db_set_cancel(struct db *db, const struct nb_cancel *cancel)
{
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	int rc = db_lock_acquire(db, &lock);

	if (rc == NB_OK)
		db->storage->cancel = cancel;

	return rc;
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
		scope->call.deadline_ns = nb_time_deadline_after_ns(
			(int64_t)db->storage->config->query_timeout_ms * INT64_C(1000000));
		scope->call.cancel = db->storage->cancel;
	}

	return rc;
}

const struct norm_call *orm_scope_call(const struct orm_scope *scope)
{
	return &scope->call;
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
			  norm_select(scope->connection, model, &selected, orm_scope_call(scope),
				      rows, &error),
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
			  norm_query_select(scope->connection, &selected, orm_scope_call(scope),
					    rows, &error),
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

	return orm_status(
		scope->database,
		norm_join_select(scope->connection, &selected, orm_scope_call(scope), rows, &error),
		&error);
}

int orm_insert_row(struct orm_scope *scope, const struct norm_model *model, const void *row,
		   const struct norm_insert_options *options, struct norm_write_result *result)
{
	struct norm_error error = {0};

	return orm_status(scope->database,
			  norm_insert(scope->connection, model, row, options, orm_scope_call(scope),
				      result, &error),
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
			  norm_update(scope->connection, model, &query, orm_scope_call(scope),
				      result, &error),
			  &error);
}

int orm_delete_rows(struct orm_scope *scope, const struct norm_model *model, struct norm_expr where,
		    struct norm_write_result *result)
{
	const struct norm_delete_query query = {.where = where};
	struct norm_error error = {0};

	return orm_status(scope->database,
			  norm_delete(scope->connection, model, &query, orm_scope_call(scope),
				      result, &error),
			  &error);
}

int orm_insert_from(struct orm_scope *scope, const struct norm_model *model, const size_t *fields,
		    size_t count, const struct norm_query *query,
		    const struct norm_conflict *conflict, struct norm_write_result *result)
{
	struct norm_error error = {0};

	return orm_status(scope->database,
			  norm_insert_select(scope->connection, model, fields, count, query,
					     conflict, orm_scope_call(scope), result, &error),
			  &error);
}

int orm_transaction_begin(struct orm_scope *scope, enum norm_sqlite_tx_mode mode)
{
	const bool postgresql = db_backend(scope->database) == DB_BACKEND_POSTGRESQL;
	const struct norm_tx_options options = {
		.sqlite_mode = postgresql ? NORM_SQLITE_DEFERRED : mode};
	struct norm_error error = {0};
	int rc = norm_tx_begin(scope->connection, &options, orm_scope_call(scope), &error);

	if (rc == NB_OK && postgresql) {
		const struct norm_assignment guard = {
			.column = norm_col(&orm_write_guard_model, 1), .value = norm_i64(0)};
		const struct norm_update_query update = {
			.set = &guard,
			.set_count = 1,
			.where = norm_eq(norm_col(&orm_write_guard_model, 0), norm_i32(1))};
		struct norm_write_result result = {0};

		rc = norm_update(scope->connection, &orm_write_guard_model, &update,
				 orm_scope_call(scope), &result, &error);
		if (rc == NB_OK && result.affected_rows != 1)
			rc = NB_ERR_STATE;

		if (rc != NB_OK)
			orm_transaction_rollback(scope);
	}

	return orm_status(scope->database, rc, &error);
}

static int orm_transaction_end(struct orm_scope *scope, bool commit)
{
	struct norm_error error = {0};
	const struct norm_call cleanup = {
		.deadline_ns = nb_time_deadline_after_ns(INT64_C(5000000000))};
	int rc = commit ? norm_tx_commit(scope->connection, orm_scope_call(scope), &error)
			: norm_tx_rollback(scope->connection, &cleanup, &error);

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
	rc = marker.owns_transaction ? orm_transaction_begin(scope, NORM_SQLITE_IMMEDIATE) : 0;
	if (rc == NB_OK)
		rc = orm_status(scope->database,
				norm_savepoint_create(scope->connection, nb_str_from_cstr(name),
						      orm_scope_call(scope), &error),
				&error);

	created = rc == NB_OK;
	if (created && !nb_vec_push_copy(&database->savepoints, &marker))
		rc = NB_ERR_NOMEM;

	if (rc != NB_OK) {
		if (marker.owns_transaction) {
			orm_transaction_rollback(scope);
		} else if (created) {
			const struct norm_call cleanup = {
				.deadline_ns = nb_time_deadline_after_ns(INT64_C(5000000000))};

			norm_savepoint_rollback(scope->connection, nb_str_from_cstr(name), &cleanup,
						NULL);
			norm_savepoint_release(scope->connection, nb_str_from_cstr(name), &cleanup,
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
	const struct norm_call cleanup = {
		.deadline_ns = nb_time_deadline_after_ns(INT64_C(5000000000))};
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
							&cleanup, &error),
				&error);

	if (rc == NB_OK)
		rc = orm_status(scope->database,
				norm_savepoint_release(scope->connection, nb_str_from_cstr(name),
						       rollback ? &cleanup : orm_scope_call(scope),
						       &error),
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
