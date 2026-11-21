#include "runtime_store.h"

#include "db/database.h"
#include "errors.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/vec.h>
#include <stdlib.h>
#include <string.h>

struct runtime_store_instance_buffer {
	nb_vec entries;
};

static void runtime_store_statement_cleanup(sqlite3_stmt **statement)
{
	sqlite3_finalize(*statement);
	*statement = NULL;
}

static void runtime_store_instance_buffer_cleanup(struct runtime_store_instance_buffer *buffer)
{
	for (size_t i = 0; i < buffer->entries.len; i++) {
		struct runtime_store_instance *item = nb_vec_at(&buffer->entries, i);

		free(item->epoch);
	}

	nb_vec_destroy(&buffer->entries);
}

void runtime_store_instances_cleanup(struct runtime_store_instances *instances)
{
	if (!instances)
		return;

	for (size_t i = 0; i < instances->count; i++)
		free(instances->items[i].epoch);

	free(instances->items);
	memset(instances, 0, sizeof(*instances));
}

int runtime_store_init(struct db *db)
{
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	int rc = db_lock_acquire(db, &lock);

	if (rc != 0)
		return rc;

	return db_exec(db,
		"CREATE TABLE IF NOT EXISTS runtime_turns("
		"turn_id TEXT PRIMARY KEY,session_id INTEGER NOT NULL "
		"REFERENCES sessions(id) ON DELETE CASCADE,owner_epoch TEXT NOT NULL,"
		"user_id TEXT NOT NULL,idempotency_key TEXT,input TEXT NOT NULL,"
		"stored_input TEXT NOT NULL,status TEXT NOT NULL,final_text TEXT NOT NULL,"
		"execution_rc INTEGER NOT NULL DEFAULT 0,persistence_rc INTEGER NOT NULL DEFAULT 0,"
		"updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now')));"
		"CREATE UNIQUE INDEX IF NOT EXISTS runtime_turns_key "
		"ON runtime_turns(session_id,user_id,idempotency_key) "
		"WHERE idempotency_key IS NOT NULL;"
		"CREATE INDEX IF NOT EXISTS runtime_turns_session "
		"ON runtime_turns(session_id,updated_at);"
		"CREATE TABLE IF NOT EXISTS runtime_instances("
		"epoch TEXT PRIMARY KEY,pid INTEGER NOT NULL,closed INTEGER NOT NULL DEFAULT 0);"
		"CREATE TABLE IF NOT EXISTS runtime_session_settings("
		"session_id INTEGER PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,"
		"access INTEGER,approval INTEGER,reviewer INTEGER,ultra INTEGER)");
}

int runtime_store_pending_instances_copy(struct db *db, struct runtime_store_instances *out)
{
	struct runtime_store_instance_buffer buffer
		NB_CLEANUP(runtime_store_instance_buffer_cleanup) = {0};
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	sqlite3_stmt *statement NB_CLEANUP(runtime_store_statement_cleanup) = NULL;

	if (!out)
		return NB_ERR_INVAL;

	if (out->items || out->count)
		return NB_ERR_EXISTS;

	int rc = db_lock_acquire(db, &lock);

	if (rc != 0)
		return rc;

	rc = sqlite3_prepare_v2(db->handle,
		"SELECT DISTINCT t.owner_epoch,i.pid,i.closed FROM runtime_turns t "
		"LEFT JOIN runtime_instances i ON i.epoch=t.owner_epoch "
		"WHERE t.status IN ('queued','running','preparing','cancelling','finishing')",
		-1, &statement, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;

	struct runtime_store_instance item = {0};

	nb_vec_init(&buffer.entries, sizeof(item));
	while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
		struct runtime_store_instance *slot = nb_vec_push(&buffer.entries);

		if (!slot)
			return NB_ERR_NOMEM;

		memset(slot, 0, sizeof(*slot));
		const char *epoch = (const char *)sqlite3_column_text(statement, 0);

		slot->epoch = strdup(epoch ? epoch : "");
		if (!slot->epoch)
			return NB_ERR_NOMEM;

		slot->process_id = sqlite3_column_int64(statement, 1);
		slot->missing = sqlite3_column_type(statement, 1) == SQLITE_NULL;
		slot->closed = sqlite3_column_int(statement, 2);
	}

	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;

	if (buffer.entries.len) {
		out->items = calloc(buffer.entries.len, sizeof(*out->items));
		if (!out->items)
			return NB_ERR_NOMEM;
	}

	out->count = buffer.entries.len;
	for (size_t i = 0; i < out->count; i++) {
		struct runtime_store_instance *source = nb_vec_at(&buffer.entries, i);

		out->items[i].epoch = nb_charp_steal(&source->epoch);
		out->items[i].process_id = source->process_id;
		out->items[i].missing = source->missing;
		out->items[i].closed = source->closed;
	}

	return 0;
}

int runtime_store_instance_register(struct db *db, const char *epoch, int64_t process_id)
{
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	sqlite3_stmt *statement NB_CLEANUP(runtime_store_statement_cleanup) = NULL;

	if (!epoch || !epoch[0] || process_id <= 0)
		return NB_ERR_INVAL;

	int rc = db_lock_acquire(db, &lock);

	if (rc != 0)
		return rc;

	rc = sqlite3_prepare_v2(db->handle,
		"INSERT OR REPLACE INTO runtime_instances(epoch,pid,closed) VALUES(?,?,0)",
		-1, &statement, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;

	sqlite3_bind_text(statement, 1, epoch, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(statement, 2, process_id);
	return sqlite3_step(statement) == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

int runtime_store_instance_close(struct db *db, const char *epoch)
{
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	sqlite3_stmt *statement NB_CLEANUP(runtime_store_statement_cleanup) = NULL;

	if (!epoch || !epoch[0])
		return NB_ERR_INVAL;

	int rc = db_lock_acquire(db, &lock);

	if (rc != 0)
		return rc;

	rc = sqlite3_prepare_v2(db->handle,
		"UPDATE runtime_instances SET closed=1 WHERE epoch=?", -1, &statement, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;

	sqlite3_bind_text(statement, 1, epoch, -1, SQLITE_TRANSIENT);
	return sqlite3_step(statement) == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

int runtime_store_recover_epoch(struct db *db, const char *epoch, int execution_rc)
{
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	sqlite3_stmt *statement NB_CLEANUP(runtime_store_statement_cleanup) = NULL;

	if (!epoch || !epoch[0])
		return NB_ERR_INVAL;

	int rc = db_lock_acquire(db, &lock);

	if (rc != 0)
		return rc;

	rc = sqlite3_prepare_v2(db->handle,
		"UPDATE runtime_turns SET status='interrupted',execution_rc=?,"
		"updated_at=strftime('%s','now') WHERE owner_epoch=? "
		"AND status IN ('queued','running','preparing','cancelling','finishing')",
		-1, &statement, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;

	sqlite3_bind_int(statement, 1, execution_rc);
	sqlite3_bind_text(statement, 2, epoch, -1, SQLITE_TRANSIENT);
	return sqlite3_step(statement) == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

int runtime_store_session_settings_get(struct db *db, int64_t session_id,
	struct runtime_store_session_settings *out)
{
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	sqlite3_stmt *statement NB_CLEANUP(runtime_store_statement_cleanup) = NULL;

	if (!out || session_id <= 0)
		return NB_ERR_INVAL;

	int rc = db_lock_acquire(db, &lock);

	if (rc != 0)
		return rc;

	rc = sqlite3_prepare_v2(db->handle,
		"SELECT access,approval,reviewer,ultra FROM runtime_session_settings "
		"WHERE session_id=?",
		-1, &statement, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;

	sqlite3_bind_int64(statement, 1, session_id);
	rc = sqlite3_step(statement);
	if (rc == SQLITE_DONE)
		return NB_ERR_NO_DATA;

	if (rc != SQLITE_ROW)
		return MORPH_ERR_DB;

	struct runtime_store_session_settings value = {
		.session_id = session_id,
		.has_permission = sqlite3_column_type(statement, 0) != SQLITE_NULL,
		.access = sqlite3_column_int(statement, 0),
		.approval = sqlite3_column_int(statement, 1),
		.reviewer = sqlite3_column_int(statement, 2),
		.has_ultra = sqlite3_column_type(statement, 3) != SQLITE_NULL,
		.ultra = sqlite3_column_int(statement, 3),
	};

	*out = value;
	return 0;
}

int runtime_store_session_settings_put(struct db *db,
	const struct runtime_store_session_settings *settings)
{
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	sqlite3_stmt *statement NB_CLEANUP(runtime_store_statement_cleanup) = NULL;

	if (!settings || settings->session_id <= 0)
		return NB_ERR_INVAL;

	int rc = db_lock_acquire(db, &lock);

	if (rc != 0)
		return rc;

	rc = sqlite3_prepare_v2(db->handle,
		"INSERT INTO runtime_session_settings(session_id,access,approval,reviewer,ultra) "
		"VALUES(?,?,?,?,?) ON CONFLICT(session_id) DO UPDATE SET "
		"access=excluded.access,approval=excluded.approval,reviewer=excluded.reviewer,"
		"ultra=excluded.ultra", -1, &statement, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;

	sqlite3_bind_int64(statement, 1, settings->session_id);
	if (settings->has_permission) {
		sqlite3_bind_int(statement, 2, settings->access);
		sqlite3_bind_int(statement, 3, settings->approval);
		sqlite3_bind_int(statement, 4, settings->reviewer);
	} else {
		sqlite3_bind_null(statement, 2);
		sqlite3_bind_null(statement, 3);
		sqlite3_bind_null(statement, 4);
	}

	if (settings->has_ultra)
		sqlite3_bind_int(statement, 5, settings->ultra);
	else
		sqlite3_bind_null(statement, 5);

	return sqlite3_step(statement) == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

int runtime_store_turn_put(struct db *db, const struct runtime_store_turn_input *turn)
{
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	sqlite3_stmt *statement NB_CLEANUP(runtime_store_statement_cleanup) = NULL;

	if (!turn || !turn->turn_id || !turn->turn_id[0] || turn->session_id <= 0 ||
	    !turn->owner_epoch || !turn->owner_epoch[0] || !turn->user_id ||
	    !turn->input || !turn->stored_input || !turn->status)
		return NB_ERR_INVAL;

	int rc = db_lock_acquire(db, &lock);

	if (rc != 0)
		return rc;

	rc = sqlite3_prepare_v2(db->handle,
		"INSERT INTO runtime_turns(turn_id,session_id,owner_epoch,user_id,"
		"idempotency_key,input,stored_input,status,final_text,execution_rc,persistence_rc) "
		"VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(turn_id) DO UPDATE SET "
		"status=excluded.status,final_text=excluded.final_text,"
		"execution_rc=excluded.execution_rc,persistence_rc=excluded.persistence_rc,"
		"updated_at=strftime('%s','now')", -1, &statement, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;

	sqlite3_bind_text(statement, 1, turn->turn_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(statement, 2, turn->session_id);
	sqlite3_bind_text(statement, 3, turn->owner_epoch, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(statement, 4, turn->user_id, -1, SQLITE_TRANSIENT);
	if (turn->idempotency_key && turn->idempotency_key[0])
		sqlite3_bind_text(statement, 5, turn->idempotency_key, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(statement, 5);

	sqlite3_bind_text(statement, 6, turn->input, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(statement, 7, turn->stored_input, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(statement, 8, turn->status, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(statement, 9, turn->final_text ? turn->final_text : "", -1,
		SQLITE_TRANSIENT);
	sqlite3_bind_int(statement, 10, turn->execution_rc);
	sqlite3_bind_int(statement, 11, turn->persistence_rc);
	return sqlite3_step(statement) == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

void runtime_store_turn_record_cleanup(struct runtime_store_turn_record *record)
{
	if (!record)
		return;

	free(record->turn_id);
	free(record->status);
	free(record->final_text);
	memset(record, 0, sizeof(*record));
}

static int runtime_store_turn_row_copy(sqlite3_stmt *statement,
	struct runtime_store_turn_record *out)
{
	struct runtime_store_turn_record value NB_CLEANUP(runtime_store_turn_record_cleanup) = {0};
	const char *id = (const char *)sqlite3_column_text(statement, 0);
	const char *status = (const char *)sqlite3_column_text(statement, 1);
	const char *text = (const char *)sqlite3_column_text(statement, 2);

	value.turn_id = strdup(id ? id : "");
	value.status = strdup(status ? status : "interrupted");
	value.final_text = strdup(text ? text : "");
	if (!value.turn_id || !value.status || !value.final_text)
		return NB_ERR_NOMEM;

	value.execution_rc = sqlite3_column_int(statement, 3);
	value.persistence_rc = sqlite3_column_int(statement, 4);
	value.session_id = sqlite3_column_int64(statement, 5);
	out->turn_id = nb_charp_steal(&value.turn_id);
	out->status = nb_charp_steal(&value.status);
	out->final_text = nb_charp_steal(&value.final_text);
	out->session_id = value.session_id;
	out->execution_rc = value.execution_rc;
	out->persistence_rc = value.persistence_rc;
	return 0;
}

static int runtime_store_turn_read(struct db *db, int64_t session_id, const char *turn_id,
	const char *user_id, const char *key, struct runtime_store_turn_record *out)
{
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	sqlite3_stmt *statement NB_CLEANUP(runtime_store_statement_cleanup) = NULL;

	if (!out || session_id <= 0)
		return NB_ERR_INVAL;

	if (out->turn_id || out->status || out->final_text)
		return NB_ERR_EXISTS;

	int rc = db_lock_acquire(db, &lock);

	if (rc != 0)
		return rc;

	const char *sql = key
		? "SELECT turn_id,status,final_text,execution_rc,persistence_rc,session_id "
		  "FROM runtime_turns WHERE session_id=? AND user_id=? AND idempotency_key=?"
		: turn_id && turn_id[0]
		? "SELECT turn_id,status,final_text,execution_rc,persistence_rc,session_id "
		  "FROM runtime_turns WHERE session_id=? AND turn_id=?"
		: "SELECT turn_id,status,final_text,execution_rc,persistence_rc,session_id "
		  "FROM runtime_turns WHERE session_id=? ORDER BY rowid DESC LIMIT 1";

	rc = sqlite3_prepare_v2(db->handle, sql, -1, &statement, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;

	sqlite3_bind_int64(statement, 1, session_id);
	if (key) {
		sqlite3_bind_text(statement, 2, user_id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(statement, 3, key, -1, SQLITE_TRANSIENT);
	} else if (turn_id && turn_id[0]) {
		sqlite3_bind_text(statement, 2, turn_id, -1, SQLITE_TRANSIENT);
	}

	rc = sqlite3_step(statement);
	if (rc == SQLITE_DONE)
		return NB_ERR_NOT_FOUND;

	if (rc != SQLITE_ROW)
		return MORPH_ERR_DB;

	return runtime_store_turn_row_copy(statement, out);
}

int runtime_store_turn_copy(struct db *db, int64_t session_id, const char *turn_id,
	struct runtime_store_turn_record *out)
{
	return runtime_store_turn_read(db, session_id, turn_id, NULL, NULL, out);
}

int runtime_store_turn_by_key_copy(struct db *db, int64_t session_id, const char *user_id,
	const char *key, struct runtime_store_turn_record *out)
{
	if (!user_id || !key || !key[0])
		return NB_ERR_INVAL;

	return runtime_store_turn_read(db, session_id, NULL, user_id, key, out);
}
