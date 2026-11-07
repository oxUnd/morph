#include "sapi/fastcgi/storage/users.h"
#include "sapi/fastcgi/session_store_internal.h"
#include "util/json.h"
#include "util/error.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include "sapi/fastcgi/security.h"
#include "util/name.h"

static int username_valid(const char *username)
{
	size_t len;

	if (!username)
		return 0;
	len = strlen(username);
	return len >= 3 && len <= 63 && morph_name_valid(username, "_-.");
}

int store_setup_required(struct session_store *s)
{
	const char *sql = "SELECT COUNT(*) FROM fcgi_users WHERE disabled=0";
	sqlite3_stmt *st = NULL;
	int count = 0;

	if (!s || sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return 1;
	if (sqlite3_step(st) == SQLITE_ROW)
		count = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return count == 0;
}

int store_create_user(struct session_store *s, const char *username, const char *password,
		      const char *role, char out_user_id[64])
{
	char user_id[64];
	char hash[192];
	const char *profile;
	const char *sql = "INSERT INTO fcgi_users"
			  "(user_id,username,password_hash,role,created_at,updated_at)"
			  " VALUES(?,?,?,?,?,?)";
	sqlite3_stmt *st = NULL;
	int64_t now = fcgi_store_now();
	int rc;

	if (!s || !username_valid(username) || !password || strlen(password) < 8)
		return -EINVAL;
	rc = fcgi_random_id("usr_", user_id, sizeof(user_id));
	if (rc < 0)
		return rc;
	rc = fcgi_password_hash(password, hash, sizeof(hash));
	if (rc < 0)
		return rc;

	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, username, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, hash, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 4, role ? role : "user", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 5, now);
	sqlite3_bind_int64(st, 6, now);
	rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc == SQLITE_CONSTRAINT)
		return -EEXIST;
	if (rc != SQLITE_DONE)
		return -EIO;

	profile = role && strcmp(role, "admin") == 0 ? "admin" : "free";
	sql = "INSERT OR REPLACE INTO fcgi_user_quota(user_id,profile)"
	      " VALUES(?,?)";
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, profile, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return -EIO;

	sql = "INSERT OR IGNORE INTO fcgi_usage_storage(user_id,used_bytes)"
	      " VALUES(?,0)";
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
		sqlite3_step(st);
		sqlite3_finalize(st);
	}
	if (out_user_id)
		snprintf(out_user_id, 64, "%s", user_id);
	return 0;
}

int store_verify_user(struct session_store *s, const char *username, const char *password,
		      struct fcgi_user *out)
{
	const char *sql = "SELECT user_id,username,password_hash,role FROM fcgi_users "
			  "WHERE username=? AND disabled=0";
	sqlite3_stmt *st = NULL;
	const char *hash;
	int ok = 0;

	if (!s || !username || !password)
		return 0;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return 0;
	sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(st) == SQLITE_ROW) {
		hash = (const char *)sqlite3_column_text(st, 2);
		if (hash && fcgi_password_verify(password, hash)) {
			if (out) {
				const char *uid = (const char *)sqlite3_column_text(st, 0);
				const char *uname = (const char *)sqlite3_column_text(st, 1);
				const char *role = (const char *)sqlite3_column_text(st, 3);
				snprintf(out->user_id, sizeof(out->user_id), "%s", uid ? uid : "");
				snprintf(out->username, sizeof(out->username), "%s",
					 uname ? uname : "");
				snprintf(out->role, sizeof(out->role), "%s", role ? role : "user");
			}
			ok = 1;
		}
	}
	sqlite3_finalize(st);
	return ok;
}
