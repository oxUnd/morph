#include "output_store.h"
#include "db/database.h"
#include "util/cleanup.h"
#include "util/error.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>

int output_store_find_id(struct db *db, const char *path, int64_t *id)
{
	sqlite3_stmt *stmt = NULL;
	int step;

	if (!db || !db->handle || !path || !id)
		return -EINVAL;
	*id = 0;
	if (sqlite3_prepare_v2(db->handle,
			       "SELECT id FROM outputs WHERE path=? ORDER BY id DESC LIMIT 1", -1,
			       &stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	step = sqlite3_step(stmt);
	if (step == SQLITE_ROW)
		*id = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	return step == SQLITE_ROW || step == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

int output_store_insert(struct db *db, const struct output_record_input *record, int64_t *id)
{
	const char *sql = "INSERT INTO outputs(session_id,kind,path,prompt,model,request_prompt,"
			  "provider,tool_name,tool_call_id,turn_id,recipe_json,mime,width,height,"
			  "duration_seconds,size_bytes,created_at) "
			  "SELECT ?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,? "
			  "WHERE NOT EXISTS(SELECT 1 FROM outputs WHERE path=?)";
	sqlite3_stmt *stmt = NULL;
	sqlite3_mutex *mutex;
	int rc;

	if (!db || !db->handle || !record || !record->path || !id)
		return -EINVAL;
	*id = 0;
	mutex = sqlite3_db_mutex(db->handle);
	sqlite3_mutex_enter(mutex);
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc == SQLITE_OK) {
		const char *strings[] = {
			record->kind,	     record->path,	     record->prompt,
			record->model,	     record->request_prompt, record->provider,
			record->tool_name,   record->tool_call_id,   record->turn_id,
			record->recipe_json, record->mime,
		};

		rc = record->session_id > 0 ? sqlite3_bind_int64(stmt, 1, record->session_id)
					    : sqlite3_bind_null(stmt, 1);
		for (size_t i = 0; rc == SQLITE_OK && i < sizeof(strings) / sizeof(strings[0]); i++)
			rc = sqlite3_bind_text(stmt, (int)i + 2, strings[i] ? strings[i] : "", -1,
					       SQLITE_TRANSIENT);
		if (rc == SQLITE_OK)
			rc = sqlite3_bind_int(stmt, 13, record->width);
		if (rc == SQLITE_OK)
			rc = sqlite3_bind_int(stmt, 14, record->height);
		if (rc == SQLITE_OK)
			rc = sqlite3_bind_int(stmt, 15, record->duration_seconds);
		if (rc == SQLITE_OK)
			rc = sqlite3_bind_int64(stmt, 16, record->size_bytes);
		if (rc == SQLITE_OK)
			rc = sqlite3_bind_int64(stmt, 17, record->created_at);
		if (rc == SQLITE_OK)
			rc = sqlite3_bind_text(stmt, 18, record->path, -1, SQLITE_TRANSIENT);
		if (rc == SQLITE_OK)
			rc = sqlite3_step(stmt);
		if (rc == SQLITE_DONE && sqlite3_changes(db->handle) > 0)
			*id = sqlite3_last_insert_rowid(db->handle);
	}
	sqlite3_finalize(stmt);
	sqlite3_mutex_leave(mutex);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

void output_record_cleanup(struct output_record *record)
{
	if (!record)
		return;
	free(record->kind);
	free(record->path);
	free(record->prompt);
	free(record->model);
	free(record->request_prompt);
	free(record->provider);
	free(record->tool_name);
	free(record->tool_call_id);
	free(record->turn_id);
	free(record->recipe_json);
	free(record->mime);
	memset(record, 0, sizeof(*record));
}

static void output_record_move(struct output_record *destination, struct output_record *source)
{
	*destination = *source;
	memset(source, 0, sizeof(*source));
}

int output_store_copy(struct db *db, const char *path, struct output_record *out)
{
	struct output_record value MORPH_CLEANUP(output_record_cleanup) = {0};
	struct output_record *record = &value;
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!db || !db->handle || !path || !out)
		return -EINVAL;
	if (out->kind || out->path || out->prompt || out->model || out->request_prompt ||
	    out->provider || out->tool_name || out->tool_call_id || out->turn_id ||
	    out->recipe_json || out->mime)
		return -EEXIST;
	rc = sqlite3_prepare_v2(
		db->handle,
		"SELECT id,session_id,kind,path,prompt,model,request_prompt,provider,"
		"tool_name,tool_call_id,turn_id,recipe_json,mime,width,height,"
		"duration_seconds,size_bytes,created_at FROM outputs "
		"WHERE path=? ORDER BY id DESC LIMIT 1",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		char **strings[] = {
			&record->kind,	      &record->path,	       &record->prompt,
			&record->model,	      &record->request_prompt, &record->provider,
			&record->tool_name,   &record->tool_call_id,   &record->turn_id,
			&record->recipe_json, &record->mime,
		};

		record->id = sqlite3_column_int64(stmt, 0);
		record->session_id = sqlite3_column_int64(stmt, 1);
		for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
			const char *text = (const char *)sqlite3_column_text(stmt, (int)i + 2);

			if (text && !(*strings[i] = strdup(text))) {
				rc = SQLITE_NOMEM;
				break;
			}
		}
		record->width = sqlite3_column_int(stmt, 13);
		record->height = sqlite3_column_int(stmt, 14);
		record->duration_seconds = sqlite3_column_int(stmt, 15);
		record->size_bytes = sqlite3_column_int64(stmt, 16);
		record->created_at = sqlite3_column_int64(stmt, 17);
	}
	sqlite3_finalize(stmt);
	if (rc == SQLITE_ROW)
		output_record_move(out, &value);
	return rc == SQLITE_ROW	    ? 0
	       : rc == SQLITE_DONE  ? -ENODATA
	       : rc == SQLITE_NOMEM ? -ENOMEM
				    : MORPH_ERR_DB;
}
