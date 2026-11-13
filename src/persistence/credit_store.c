#include "credit_store.h"
#include "errors.h"
#include <errno.h>
#include <sqlite3.h>
#include <string.h>
#include <time.h>

int credit_store_init_schema(struct db *db)
{
	if (!db || !db->handle)
		return NB_ERR_INVAL;
	return db_init_credit_schema(db);
}

int credit_store_record_event(struct db *db,
			      const struct credit_store_event *event)
{
	const char *sql =
		"INSERT INTO credit_events"
		"(user_id,session_id,kind,provider,model,input_tokens,"
		"output_tokens,image_units,video_seconds,estimated_cost,"
		"currency,credits,metadata_json,created_at)"
		" VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!db || !db->handle || !event || !event->kind)
		return NB_ERR_INVAL;

	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, event->user_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, event->session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, event->kind, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, event->provider, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, event->model, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 6, event->input_tokens);
	sqlite3_bind_int64(stmt, 7, event->output_tokens);
	sqlite3_bind_int64(stmt, 8, event->image_units);
	sqlite3_bind_int64(stmt, 9, event->video_seconds);
	sqlite3_bind_double(stmt, 10, event->estimated_cost);
	sqlite3_bind_text(stmt, 11, event->currency, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 12, event->credits);
	sqlite3_bind_text(stmt, 13, event->metadata_json, -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 14, (int64_t)time(NULL));
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	return 0;
}

static int credit_store_summary_query(struct db *db, const char *sql,
				      const char *value,
				      struct credit_store_summary *out)
{
	sqlite3_stmt *stmt = NULL;

	if (!db || !db->handle || !sql || !value || !out)
		return NB_ERR_INVAL;
	memset(out, 0, sizeof(*out));
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		out->credits = sqlite3_column_int64(stmt, 0);
		out->estimated_cost = sqlite3_column_double(stmt, 1);
		out->event_count = sqlite3_column_int(stmt, 2);
	}
	sqlite3_finalize(stmt);
	return 0;
}

int credit_store_summary_today(struct db *db, const char *user_id,
			       struct credit_store_summary *out)
{
	const char *sql =
		"SELECT COALESCE(SUM(credits),0),"
		"COALESCE(SUM(estimated_cost),0),COUNT(*) "
		"FROM credit_events WHERE user_id=? "
		"AND created_at >= strftime('%s','now','start of day')";

	return credit_store_summary_query(db, sql, user_id, out);
}

int credit_store_summary_total(struct db *db, const char *user_id,
			       struct credit_store_summary *out)
{
	const char *sql =
		"SELECT COALESCE(SUM(credits),0),"
		"COALESCE(SUM(estimated_cost),0),COUNT(*) "
		"FROM credit_events WHERE user_id=?";

	return credit_store_summary_query(db, sql, user_id, out);
}

int credit_store_summary_session(struct db *db, const char *session_id,
				 struct credit_store_summary *out)
{
	const char *sql =
		"SELECT COALESCE(SUM(credits),0),"
		"COALESCE(SUM(estimated_cost),0),COUNT(*) "
		"FROM credit_events WHERE session_id=?";

	return credit_store_summary_query(db, sql, session_id, out);
}
