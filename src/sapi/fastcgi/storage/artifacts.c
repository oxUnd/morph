#include "sapi/fastcgi/storage/artifacts.h"
#include "util/cleanup.h"
#include "sapi/fastcgi/session_store_internal.h"
#include "cJSON.h"
#include "util/error.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include "sapi/fastcgi/security.h"

static char *artifact_json_array(sqlite3_stmt *st)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *items = cJSON_CreateArray();
	char *json;

	if (!root || !items) {
		cJSON_Delete(root);
		return NULL;
	}
	cJSON_AddItemToObject(root, "items", items);
	while (sqlite3_step(st) == SQLITE_ROW) {
		cJSON *obj = cJSON_CreateObject();
		const char *id = (const char *)sqlite3_column_text(st, 0);
		char url[128];
		if (!obj) {
			cJSON_Delete(root);
			return NULL;
		}
		snprintf(url, sizeof(url), "/api/artifacts/%s", id ? id : "");
		cJSON_AddStringToObject(obj, "id", id ? id : "");
		cJSON_AddStringToObject(obj, "session_id",
					(const char *)sqlite3_column_text(st, 1));
		cJSON_AddStringToObject(obj, "kind", (const char *)sqlite3_column_text(st, 2));
		cJSON_AddStringToObject(obj, "mime", (const char *)sqlite3_column_text(st, 3));
		cJSON_AddStringToObject(obj, "filename", (const char *)sqlite3_column_text(st, 4));
		cJSON_AddNumberToObject(obj, "size_bytes", (double)sqlite3_column_int64(st, 5));
		cJSON_AddStringToObject(obj, "status", (const char *)sqlite3_column_text(st, 6));
		cJSON_AddNumberToObject(obj, "created_at", (double)sqlite3_column_int64(st, 7));
		cJSON_AddStringToObject(obj, "url", url);
		cJSON_AddItemToArray(items, obj);
	}
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return json;
}

static int artifact_quota_allows(struct session_store *s, const char *user_id, const char *kind,
				 int64_t size_bytes)
{
	const char *sql = "SELECT p.daily_images,p.daily_videos,p.storage_bytes,"
			  "COALESCE(d.images,0),COALESCE(d.videos,0),"
			  "COALESCE(st.used_bytes,0) "
			  "FROM fcgi_user_quota q "
			  "JOIN fcgi_quota_profiles p ON p.name=q.profile "
			  "LEFT JOIN fcgi_usage_daily d ON d.user_id=q.user_id "
			  " AND d.day=date('now') "
			  "LEFT JOIN fcgi_usage_storage st ON st.user_id=q.user_id "
			  "WHERE q.user_id=?";
	sqlite3_stmt *st = NULL;
	int64_t image_limit;
	int64_t video_limit;
	int64_t storage_limit;
	int64_t image_used;
	int64_t video_used;
	int64_t storage_used;
	int rc = 0;

	if (!s || !user_id || !kind || size_bytes < 0)
		return -EINVAL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(st) == SQLITE_ROW) {
		image_limit = sqlite3_column_int64(st, 0);
		video_limit = sqlite3_column_int64(st, 1);
		storage_limit = sqlite3_column_int64(st, 2);
		image_used = sqlite3_column_int64(st, 3);
		video_used = sqlite3_column_int64(st, 4);
		storage_used = sqlite3_column_int64(st, 5);
		if (strcmp(kind, "image") == 0 && image_limit >= 0 && image_used >= image_limit)
			rc = -EDQUOT;
		else if (strcmp(kind, "video") == 0 && video_limit >= 0 &&
			 video_used >= video_limit)
			rc = -EDQUOT;
		else if (storage_limit >= 0 && (storage_used > storage_limit ||
						size_bytes > storage_limit - storage_used))
			rc = -EDQUOT;
	}
	sqlite3_finalize(st);
	return rc;
}

static void artifact_statement_cleanup(sqlite3_stmt **owner)
{
	sqlite3_finalize(*owner);
	*owner = NULL;
}

static int store_artifact_register_write(struct session_store *s, const char *user_id,
	const char *session_id, const char *kind, const char *mime, const char *filename,
	const char *relative_path, int64_t size_bytes, char *id, size_t id_size)
{
	const char *sql = "INSERT INTO fcgi_artifacts"
			  "(id,user_id,session_id,kind,mime,filename,relative_path,"
			  "size_bytes,status,created_at)"
			  " VALUES(?,?,?,?,?,?,?,?, 'ready', ?)";
	sqlite3_stmt *st MORPH_CLEANUP(artifact_statement_cleanup) = NULL;
	int rc;

	rc = artifact_quota_allows(s, user_id, kind, size_bytes);
	if (rc != 0)
		return rc;
	rc = fcgi_random_id("art_", id, id_size);
	if (rc < 0)
		return rc;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK) {
		rc = -EIO;
		return rc;
	}
	sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, user_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 4, kind, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 5, mime, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 6, filename, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 7, relative_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 8, size_bytes);
	sqlite3_bind_int64(st, 9, fcgi_store_now());
	rc = sqlite3_step(st);
	artifact_statement_cleanup(&st);
	if (rc != SQLITE_DONE) {
		rc = -EIO;
		return rc;
	}

	sql = "INSERT INTO fcgi_usage_storage(user_id,used_bytes)"
	      " VALUES(?,?) "
	      "ON CONFLICT(user_id) DO UPDATE SET "
	      "used_bytes=used_bytes+excluded.used_bytes";
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK) {
		rc = -EIO;
		return rc;
	}
	sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 2, size_bytes);
	rc = sqlite3_step(st);
	artifact_statement_cleanup(&st);
	if (rc != SQLITE_DONE) {
		rc = -EIO;
		return rc;
	}
	if (strcmp(kind, "image") == 0 || strcmp(kind, "video") == 0) {
		const char *col = strcmp(kind, "image") == 0 ? "images" : "videos";
		char usage_sql[256];
		snprintf(usage_sql, sizeof(usage_sql),
			 "INSERT INTO fcgi_usage_daily(user_id,day,%s)"
			 " VALUES(?,date('now'),1) "
			 "ON CONFLICT(user_id,day) DO UPDATE SET %s=%s+1",
			 col, col, col);
		if (sqlite3_prepare_v2(s->db.handle, usage_sql, -1, &st, NULL) != SQLITE_OK) {
			rc = -EIO;
			return rc;
		}
		sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
		rc = sqlite3_step(st);
		artifact_statement_cleanup(&st);
		if (rc != SQLITE_DONE) {
			rc = -EIO;
			return rc;
		}
	}
	return 0;
}

int store_artifact_register(struct session_store *s, const char *user_id, const char *session_id,
			    const char *kind, const char *mime, const char *filename,
			    const char *relative_path, int64_t size_bytes, char out_artifact_id[64])
{
	char id[64];
	int rc;

	if (!s || !user_id || !session_id || !kind || !mime || !filename || !relative_path)
		return -EINVAL;
	/* Keep the quota check and both usage records in one transaction. */
	rc = store_transaction_begin(s);
	if (rc != 0)
		return rc;
	rc = store_artifact_register_write(s, user_id, session_id, kind, mime, filename,
					   relative_path, size_bytes, id, sizeof(id));
	rc = store_transaction_end(s, rc);
	if (rc != 0)
		return rc;
	if (out_artifact_id)
		snprintf(out_artifact_id, 64, "%s", id);
	return 0;
}

int store_artifact_get(struct session_store *s, const char *artifact_id, const char *user_id,
		       struct artifact_record *out)
{
	const char *sql = "SELECT id,user_id,session_id,kind,mime,filename,"
			  "relative_path,size_bytes,status,created_at "
			  "FROM fcgi_artifacts WHERE id=? AND user_id=?";
	sqlite3_stmt *st = NULL;
	int found = 0;

	if (!s || !artifact_id || !user_id || !out)
		return -EINVAL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(st, 1, artifact_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, user_id, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(st) == SQLITE_ROW) {
		memset(out, 0, sizeof(*out));
		snprintf(out->id, sizeof(out->id), "%s", (const char *)sqlite3_column_text(st, 0));
		snprintf(out->user_id, sizeof(out->user_id), "%s",
			 (const char *)sqlite3_column_text(st, 1));
		snprintf(out->session_id, sizeof(out->session_id), "%s",
			 (const char *)sqlite3_column_text(st, 2));
		snprintf(out->kind, sizeof(out->kind), "%s",
			 (const char *)sqlite3_column_text(st, 3));
		snprintf(out->mime, sizeof(out->mime), "%s",
			 (const char *)sqlite3_column_text(st, 4));
		snprintf(out->filename, sizeof(out->filename), "%s",
			 (const char *)sqlite3_column_text(st, 5));
		snprintf(out->relative_path, sizeof(out->relative_path), "%s",
			 (const char *)sqlite3_column_text(st, 6));
		out->size_bytes = sqlite3_column_int64(st, 7);
		snprintf(out->status, sizeof(out->status), "%s",
			 (const char *)sqlite3_column_text(st, 8));
		out->created_at = sqlite3_column_int64(st, 9);
		found = 1;
	}
	sqlite3_finalize(st);
	return found ? 0 : -ENOENT;
}

int store_artifact_list_json(struct session_store *s, const char *user_id, const char *session_id,
			     char **out_json)
{
	const char *sql = "SELECT id,session_id,kind,mime,filename,size_bytes,status,"
			  "created_at FROM fcgi_artifacts "
			  "WHERE user_id=? AND session_id=? ORDER BY created_at DESC";
	sqlite3_stmt *st = NULL;
	char *json;

	if (!s || !user_id || !session_id || !out_json)
		return -EINVAL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, session_id, -1, SQLITE_TRANSIENT);
	json = artifact_json_array(st);
	sqlite3_finalize(st);
	if (!json)
		return -ENOMEM;
	*out_json = json;
	return 0;
}
