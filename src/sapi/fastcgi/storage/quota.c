#include "sapi/fastcgi/storage/quota.h"
#include "sapi/fastcgi/session_store_internal.h"
#include "cJSON.h"
#include "errors.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include "sapi/fastcgi/security.h"

static void add_limit_obj(cJSON *parent, const char *name, int used, int limit)
{
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddNumberToObject(obj, "used", used);
	cJSON_AddNumberToObject(obj, "limit", limit);
	cJSON_AddItemToObject(parent, name, obj);
}

int store_user_quota_json(struct session_store *s, const char *user_id, char **out_json)
{
	const char *sql = "SELECT q.profile,p.daily_turns,p.daily_images,p.daily_videos,"
			  "p.storage_bytes,p.max_concurrent_turns,"
			  "COALESCE(d.turns,0),COALESCE(d.images,0),"
			  "COALESCE(d.videos,0),COALESCE(st.used_bytes,0) "
			  "FROM fcgi_user_quota q "
			  "JOIN fcgi_quota_profiles p ON p.name=q.profile "
			  "LEFT JOIN fcgi_usage_daily d ON d.user_id=q.user_id "
			  " AND d.day=date('now') "
			  "LEFT JOIN fcgi_usage_storage st ON st.user_id=q.user_id "
			  "WHERE q.user_id=?";
	sqlite3_stmt *stmt = NULL;
	cJSON *root = NULL;
	cJSON *daily = NULL;
	cJSON *storage = NULL;
	cJSON *conc = NULL;
	cJSON *credits = NULL;
	char *json = NULL;

	if (!s || !user_id || !out_json)
		return NB_ERR_INVAL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		return NB_ERR_IO;
	sqlite3_bind_text(stmt, 1, user_id, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) != SQLITE_ROW) {
		sqlite3_finalize(stmt);
		return NB_ERR_NOT_FOUND;
	}

	root = cJSON_CreateObject();
	daily = cJSON_CreateObject();
	storage = cJSON_CreateObject();
	conc = cJSON_CreateObject();
	credits = cJSON_CreateObject();
	if (!root || !daily || !storage || !conc || !credits) {
		cJSON_Delete(root);
		cJSON_Delete(daily);
		cJSON_Delete(storage);
		cJSON_Delete(conc);
		cJSON_Delete(credits);
		sqlite3_finalize(stmt);
		return NB_ERR_NOMEM;
	}
	cJSON_AddStringToObject(root, "profile", (const char *)sqlite3_column_text(stmt, 0));
	add_limit_obj(daily, "turns", sqlite3_column_int(stmt, 6), sqlite3_column_int(stmt, 1));
	add_limit_obj(daily, "images", sqlite3_column_int(stmt, 7), sqlite3_column_int(stmt, 2));
	add_limit_obj(daily, "videos", sqlite3_column_int(stmt, 8), sqlite3_column_int(stmt, 3));
	cJSON_AddItemToObject(root, "daily", daily);
	cJSON_AddNumberToObject(storage, "used_bytes", (double)sqlite3_column_int64(stmt, 9));
	cJSON_AddNumberToObject(storage, "limit_bytes", (double)sqlite3_column_int64(stmt, 4));
	cJSON_AddItemToObject(root, "storage", storage);
	cJSON_AddNumberToObject(conc, "used", 0);
	cJSON_AddNumberToObject(conc, "limit", sqlite3_column_int(stmt, 5));
	cJSON_AddItemToObject(root, "concurrent_turns", conc);
	{
		const char *csql = "SELECT COALESCE(SUM(credits),0),"
				   "COALESCE(SUM(estimated_cost),0),COUNT(*),"
				   "COALESCE(MAX(currency),'USD') "
				   "FROM credit_events WHERE user_id=? "
				   "AND created_at >= strftime('%s','now','start of day')";
		sqlite3_stmt *cst = NULL;
		if (sqlite3_prepare_v2(s->db.handle, csql, -1, &cst, NULL) == SQLITE_OK) {
			sqlite3_bind_text(cst, 1, user_id, -1, SQLITE_TRANSIENT);
			if (sqlite3_step(cst) == SQLITE_ROW) {
				cJSON_AddNumberToObject(credits, "used_today",
							(double)sqlite3_column_int64(cst, 0));
				cJSON_AddNumberToObject(credits, "estimated_cost_today",
							sqlite3_column_double(cst, 1));
				cJSON_AddNumberToObject(credits, "events_today",
							sqlite3_column_int(cst, 2));
				cJSON_AddStringToObject(credits, "currency",
							(const char *)sqlite3_column_text(cst, 3));
			}
			sqlite3_finalize(cst);
		}
		cJSON_AddNumberToObject(credits, "daily_limit", -1);
		cJSON_AddBoolToObject(credits, "over_limit", 0);
		cJSON_AddItemToObject(root, "credits", credits);
	}

	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	sqlite3_finalize(stmt);
	if (!json)
		return NB_ERR_NOMEM;
	*out_json = json;
	return 0;
}

int store_quota_begin_turn(struct session_store *s, const char *user_id, const char *session_id,
			   char out_turn_id[64])
{
	const char *qsql = "SELECT p.daily_turns,p.max_concurrent_turns,"
			   "COALESCE(d.turns,0),"
			   "(SELECT COUNT(*) FROM fcgi_running_turns WHERE user_id=?) "
			   "FROM fcgi_user_quota q "
			   "JOIN fcgi_quota_profiles p ON p.name=q.profile "
			   "LEFT JOIN fcgi_usage_daily d ON d.user_id=q.user_id "
			   " AND d.day=date('now') WHERE q.user_id=?";
	const char *usql = "INSERT INTO fcgi_usage_daily(user_id,day,turns)"
			   " VALUES(?,date('now'),1) "
			   "ON CONFLICT(user_id,day) DO UPDATE SET turns=turns+1";
	const char *isql = "INSERT INTO fcgi_running_turns"
			   "(id,user_id,session_id,started_at,heartbeat_at)"
			   " VALUES(?,?,?,?,?)";
	sqlite3_stmt *st = NULL;
	char turn_id[64];
	int daily_limit;
	int concurrent_limit;
	int used_turns;
	int running;
	int rc;
	int64_t now = fcgi_store_now();

	if (!s || !user_id || !session_id)
		return NB_ERR_INVAL;
	rc = fcgi_random_id("turn_", turn_id, sizeof(turn_id));
	if (rc < 0)
		return rc;

	rc = store_transaction_begin(s);
	if (rc != 0)
		return rc;
	if (sqlite3_prepare_v2(s->db.handle, qsql, -1, &st, NULL) != SQLITE_OK)
		return store_transaction_end(s, NB_ERR_IO);
	sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, user_id, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(st);
	if (rc != SQLITE_ROW) {
		sqlite3_finalize(st);
		return store_transaction_end(s, rc == SQLITE_DONE ? NB_ERR_NOT_FOUND : NB_ERR_IO);
	}
	daily_limit = sqlite3_column_int(st, 0);
	concurrent_limit = sqlite3_column_int(st, 1);
	used_turns = sqlite3_column_int(st, 2);
	running = sqlite3_column_int(st, 3);
	sqlite3_finalize(st);

	if (daily_limit >= 0 && used_turns >= daily_limit)
		return store_transaction_end(s, NB_ERR_NO_SPACE);
	if (concurrent_limit >= 0 && running >= concurrent_limit)
		return store_transaction_end(s, NB_ERR_AGAIN);

	if (sqlite3_prepare_v2(s->db.handle, usql, -1, &st, NULL) != SQLITE_OK)
		return store_transaction_end(s, NB_ERR_IO);
	sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return store_transaction_end(s, NB_ERR_IO);

	if (sqlite3_prepare_v2(s->db.handle, isql, -1, &st, NULL) != SQLITE_OK)
		return store_transaction_end(s, NB_ERR_IO);
	sqlite3_bind_text(st, 1, turn_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, user_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 4, now);
	sqlite3_bind_int64(st, 5, now);
	rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return store_transaction_end(s, NB_ERR_IO);

	rc = store_transaction_end(s, 0);
	if (rc != 0)
		return rc;
	if (out_turn_id)
		snprintf(out_turn_id, 64, "%s", turn_id);
	return 0;
}

void store_quota_end_turn(struct session_store *s, const char *turn_id)
{
	const char *sql = "DELETE FROM fcgi_running_turns WHERE id=?";
	sqlite3_stmt *st = NULL;

	if (!s || !turn_id || !*turn_id)
		return;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, turn_id, -1, SQLITE_TRANSIENT);
		sqlite3_step(st);
		sqlite3_finalize(st);
	}
}

void store_turn_heartbeat(struct session_store *s, const char *turn_id)
{
	const char *sql = "UPDATE fcgi_running_turns SET heartbeat_at=? WHERE id=?";
	sqlite3_stmt *st = NULL;

	if (!s || !turn_id || !*turn_id)
		return;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return;
	sqlite3_bind_int64(st, 1, fcgi_store_now());
	sqlite3_bind_text(st, 2, turn_id, -1, SQLITE_TRANSIENT);
	sqlite3_step(st);
	sqlite3_finalize(st);
}
