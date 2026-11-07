#include "memory_store.h"

#include "util/error.h"

#include <errno.h>

static int memory_store_exec_delete(struct db *db, const char *sql, int64_t session_id)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!db || !db->handle || !sql)
		return -EINVAL;
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

int memory_store_clear_all(struct db *db, int64_t session_id)
{
	int rc;

	rc = memory_store_clear_facts(db, session_id);
	if (rc != 0)
		return rc;
	rc = memory_store_clear_episodes(db, session_id);
	if (rc != 0)
		return rc;
	return memory_store_clear_procedures(db, session_id);
}

int memory_store_clear_facts(struct db *db, int64_t session_id)
{
	int rc;

	rc = memory_store_exec_delete(db, "DELETE FROM memory_profiles WHERE session_id=?",
				      session_id);
	if (rc != 0)
		return rc;
	return memory_store_exec_delete(db, "DELETE FROM memory_facts WHERE session_id=?",
					session_id);
}

int memory_store_clear_episodes(struct db *db, int64_t session_id)
{
	return memory_store_exec_delete(db, "DELETE FROM memory_episodes WHERE session_id=?",
					session_id);
}

int memory_store_clear_procedures(struct db *db, int64_t session_id)
{
	return memory_store_exec_delete(db, "DELETE FROM memory_procedures WHERE session_id=?",
					session_id);
}
