#include "permission_grant.h"
#include "util/error.h"
#include "util/log.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

int permission_grant_save(struct db *db,
			  const struct permission_grant *grant)
{
	static const char *sql =
		"INSERT OR IGNORE INTO permission_grants("
		"subject,resource_kind,resource,project_root,created_at)"
		"VALUES(?,?,?,?,strftime('%s','now'))";
	sqlite3_stmt *stmt = NULL;
	int rc = 0;

	if (!db || !db->handle || !grant || !grant->subject[0] ||
	    !grant->resource_kind[0] || !grant->resource[0] ||
	    !grant->project_root[0])
		MORPH_RETURN(-EINVAL);
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
		rc = MORPH_ERR_DB;
		goto out;
	}
	if (sqlite3_bind_text(stmt, 1, grant->subject, -1,
			      SQLITE_TRANSIENT) != SQLITE_OK ||
	    sqlite3_bind_text(stmt, 2, grant->resource_kind, -1,
			      SQLITE_TRANSIENT) != SQLITE_OK ||
	    sqlite3_bind_text(stmt, 3, grant->resource, -1,
			      SQLITE_TRANSIENT) != SQLITE_OK ||
	    sqlite3_bind_text(stmt, 4, grant->project_root, -1,
			      SQLITE_TRANSIENT) != SQLITE_OK) {
		rc = MORPH_ERR_DB;
		goto out;
	}
	if (sqlite3_step(stmt) != SQLITE_DONE)
		rc = MORPH_ERR_DB;

out:
	if (rc != 0)
		log_warn("permission grant save failed: %s",
			 sqlite3_errmsg(db->handle));
	sqlite3_finalize(stmt);
	return rc;
}

int permission_grant_each(struct db *db, const char *project_root,
			  permission_grant_iter_fn fn, void *user_data)
{
	static const char *sql =
		"SELECT subject,resource_kind,resource,project_root "
		"FROM permission_grants WHERE project_root=? ORDER BY id";
	struct permission_grant grant;
	sqlite3_stmt *stmt = NULL;
	int step;
	int rc = 0;

	if (!db || !db->handle || !project_root || !*project_root || !fn)
		MORPH_RETURN(-EINVAL);
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	if (sqlite3_bind_text(stmt, 1, project_root, -1,
			      SQLITE_TRANSIENT) != SQLITE_OK) {
		rc = MORPH_ERR_DB;
		goto out;
	}
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *subject =
			(const char *)sqlite3_column_text(stmt, 0);
		const char *kind =
			(const char *)sqlite3_column_text(stmt, 1);
		const char *resource =
			(const char *)sqlite3_column_text(stmt, 2);
		const char *project =
			(const char *)sqlite3_column_text(stmt, 3);

		memset(&grant, 0, sizeof(grant));
		snprintf(grant.subject, sizeof(grant.subject), "%s",
			 subject ? subject : "");
		snprintf(grant.resource_kind, sizeof(grant.resource_kind), "%s",
			 kind ? kind : "");
		snprintf(grant.resource, sizeof(grant.resource), "%s",
			 resource ? resource : "");
		snprintf(grant.project_root, sizeof(grant.project_root), "%s",
			 project ? project : "");
		rc = fn(&grant, user_data);
		if (rc != 0)
			goto out;
	}
	if (step != SQLITE_DONE)
		rc = MORPH_ERR_DB;

out:
	sqlite3_finalize(stmt);
	return rc;
}
