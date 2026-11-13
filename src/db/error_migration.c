#include "error_migration.h"
#include "database.h"
#include "errors.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include <stdbool.h>
#include <string.h>

struct error_migration_savepoint {
	struct db *db;
	bool active;
};

static void error_migration_rollback(struct error_migration_savepoint *savepoint)
{
	if (savepoint->active)
		(void)db_exec(savepoint->db,
			      "ROLLBACK TO nbase_error_codes; RELEASE nbase_error_codes;");
}

static int error_migration_release(struct error_migration_savepoint *savepoint)
{
	int rc = db_exec(savepoint->db, "RELEASE nbase_error_codes;");

	if (rc == 0)
		savepoint->active = false;
	return rc;
}

static void error_migration_statement_cleanup(sqlite3_stmt **statement)
{
	if (*statement)
		sqlite3_finalize(*statement);
}

static int error_migration_has_column(struct db *db, const char *table, bool *has)
{
	nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};
	sqlite3_stmt *statement NB_CLEANUP(error_migration_statement_cleanup) = NULL;

	*has = false;
	if (!nb_buf_printf(&sql, "PRAGMA table_info(%s)", table))
		return nb_buf_status(&sql);
	if (sqlite3_prepare_v2(db->handle, nb_buf_cstr(&sql), -1, &statement, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	int rc;
	while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
		const char *name = (const char *)sqlite3_column_text(statement, 1);
		if (name && strcmp(name, "legacy_error_code") == 0)
			*has = true;
	}
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

int db_error_migration_append_status_sql(nb_buf *sql, const char *value)
{
	/* Historical domain IDs are fixed. Errno numbers after the common Linux/
	 * Darwin prefix differ, and the old database did not record its source OS.
	 * Only unambiguous common errno values receive a more specific status. */
	static const struct {
		int legacy;
		int current;
	} mappings[] = {
		{-257, MORPH_ERR_NOT_CONFIGURED},
		{-258, MORPH_ERR_NOT_INITIALIZED},
		{-259, MORPH_ERR_API},
		{-260, MORPH_ERR_NETWORK},
		{-261, NB_ERR_PARSE},
		{-262, NB_ERR_PROTOCOL},
		{-263, MORPH_ERR_DB},
		{-264, NB_ERR_FORMAT},
		{-265, MORPH_ERR_PROCESSING},
		{-266, MORPH_ERR_SANDBOX},
		{-267, MORPH_ERR_LOAD},
		{-268, MORPH_ERR_LLM},
		{-269, MORPH_ERR_REACT_MAX_ITERATIONS},
		{-270, MORPH_ERR_CONFIG},
		{-271, MORPH_ERR_CONTEXT_LIMIT},
		{-272, MORPH_ERR_TOKEN_BUDGET},
		{-273, MORPH_ERR_AGENT_BUDGET},
		{-274, MORPH_ERR_REQUEST_BUDGET},
		{-1, NB_ERR_NOT_PERMITTED},
		{-2, NB_ERR_NOT_FOUND},
		{-4, NB_ERR_INTERRUPTED},
		{-5, NB_ERR_IO},
		{-7, NB_ERR_TOO_BIG},
		{-9, NB_ERR_BAD_HANDLE},
		{-10, NB_ERR_NO_CHILD},
		{-12, NB_ERR_NOMEM},
		{-13, NB_ERR_PERMISSION},
		{-16, NB_ERR_BUSY},
		{-17, NB_ERR_EXISTS},
		{-18, NB_ERR_CROSS_DEVICE},
		{-20, NB_ERR_NOT_DIR},
		{-21, NB_ERR_IS_DIR},
		{-22, NB_ERR_INVAL},
		{-24, NB_ERR_RESOURCE},
		{-25, NB_ERR_NOT_TERMINAL},
		{-27, NB_ERR_TOO_BIG},
		{-28, NB_ERR_NO_SPACE},
		{-32, NB_ERR_BROKEN_PIPE},
		{-34, NB_ERR_RANGE},
	};
	if (!sql || !value)
		return NB_ERR_INVAL;
	(void)nb_buf_printf(sql, "CASE %s", value);
	for (size_t index = 0; index < sizeof(mappings) / sizeof(mappings[0]); index++)
		(void)nb_buf_printf(sql, " WHEN %d THEN %d", mappings[index].legacy,
				    mappings[index].current);
	(void)nb_buf_printf(sql, " ELSE CASE WHEN %s>=0 THEN %s ELSE %d END END", value, value,
			    NB_ERR_OS);
	return nb_buf_status(sql);
}

static int error_migration_table(struct db *db, const char *table)
{
	nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};
	bool has = false;
	int rc = error_migration_has_column(db, table, &has);

	if (rc != 0)
		return rc;
	if (!has) {
		if (!nb_buf_printf(&sql, "ALTER TABLE %s ADD COLUMN legacy_error_code INTEGER",
				   table))
			return nb_buf_status(&sql);
		rc = db_exec(db, nb_buf_cstr(&sql));
		if (rc != 0)
			return rc;
		nb_buf_clear(&sql);
	}
	(void)nb_buf_printf(&sql, "UPDATE %s SET legacy_error_code=error_code,error_code=", table);
	rc = db_error_migration_append_status_sql(&sql, "error_code");
	return rc != 0 ? rc : db_exec(db, nb_buf_cstr(&sql));
}

int db_migrate_error_codes(struct db *db)
{
	static const char *const tables[] = {
		"memory_jobs",
		"sub_agent_tasks",
		"agent_threads",
		"history_compaction_attempts",
	};
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	struct error_migration_savepoint savepoint NB_CLEANUP(error_migration_rollback) = {
		.db = db};
	sqlite3_stmt *statement NB_CLEANUP(error_migration_statement_cleanup) = NULL;
	int rc = db_lock_acquire(db, &lock);

	if (rc != 0)
		return rc;
	rc = db_exec(db, "SAVEPOINT nbase_error_codes;");
	if (rc != 0)
		return rc;
	savepoint.active = true;
	if (sqlite3_prepare_v2(db->handle, "SELECT 1 FROM schema_migrations WHERE version=3", -1,
			       &statement, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	rc = sqlite3_step(statement);
	if (rc == SQLITE_ROW) {
		sqlite3_finalize(statement);
		statement = NULL;
		return error_migration_release(&savepoint);
	}
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	sqlite3_finalize(statement);
	statement = NULL;
	for (size_t index = 0; index < sizeof(tables) / sizeof(tables[0]); index++) {
		rc = error_migration_table(db, tables[index]);
		if (rc != 0)
			return rc;
	}
	rc = db_exec(db, "INSERT INTO schema_migrations(version,name,applied_at) "
			 "VALUES(3,'nbase_error_codes',strftime('%s','now'));");
	if (rc != 0)
		return rc;
	return error_migration_release(&savepoint);
}
