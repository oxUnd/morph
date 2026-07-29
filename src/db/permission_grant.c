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
		"SELECT id,subject,resource_kind,resource,project_root,created_at "
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
			(const char *)sqlite3_column_text(stmt, 1);
		const char *kind =
			(const char *)sqlite3_column_text(stmt, 2);
		const char *resource =
			(const char *)sqlite3_column_text(stmt, 3);
		const char *project =
			(const char *)sqlite3_column_text(stmt, 4);

		memset(&grant, 0, sizeof(grant));
		grant.id = sqlite3_column_int64(stmt, 0);
		snprintf(grant.subject, sizeof(grant.subject), "%s",
			 subject ? subject : "");
		snprintf(grant.resource_kind, sizeof(grant.resource_kind), "%s",
			 kind ? kind : "");
		snprintf(grant.resource, sizeof(grant.resource), "%s",
			 resource ? resource : "");
		snprintf(grant.project_root, sizeof(grant.project_root), "%s",
			 project ? project : "");
		grant.created_at = sqlite3_column_int64(stmt, 5);
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

static int delete_grants(struct db *db, const char *sql,
			 const char *project_root, const char *subject,
			 int64_t id, int *deleted)
{
	sqlite3_stmt *stmt = NULL;
	int bind_index = 1;
	int rc = 0;

	if (!db || !db->handle || !sql || !deleted)
		MORPH_RETURN(-EINVAL);
	*deleted = 0;
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	if (project_root &&
	    sqlite3_bind_text(stmt, bind_index++, project_root, -1,
			      SQLITE_TRANSIENT) != SQLITE_OK) {
		rc = MORPH_ERR_DB;
		goto out;
	}
	if (subject &&
	    sqlite3_bind_text(stmt, bind_index++, subject, -1,
			      SQLITE_TRANSIENT) != SQLITE_OK) {
		rc = MORPH_ERR_DB;
		goto out;
	}
	if (id > 0 && sqlite3_bind_int64(stmt, bind_index, id) != SQLITE_OK) {
		rc = MORPH_ERR_DB;
		goto out;
	}
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		rc = MORPH_ERR_DB;
		goto out;
	}
	*deleted = sqlite3_changes(db->handle);

out:
	if (rc != 0)
		log_warn("permission grant delete failed: %s",
			 sqlite3_errmsg(db->handle));
	sqlite3_finalize(stmt);
	return rc;
}

int permission_grant_delete_id(struct db *db, const char *project_root,
			       int64_t id, int *deleted)
{
	if (!project_root || !*project_root || id <= 0)
		MORPH_RETURN(-EINVAL);
	return delete_grants(db,
		"DELETE FROM permission_grants WHERE project_root=? AND id=?",
		project_root, NULL, id, deleted);
}

int permission_grant_delete_subject(struct db *db, const char *project_root,
				    const char *subject, int *deleted)
{
	if (!project_root || !*project_root || !subject || !*subject)
		MORPH_RETURN(-EINVAL);
	return delete_grants(db,
		"DELETE FROM permission_grants "
		"WHERE project_root=? AND subject=?",
		project_root, subject, 0, deleted);
}

int permission_grant_clear_project(struct db *db, const char *project_root,
				   int *deleted)
{
	if (!project_root || !*project_root)
		MORPH_RETURN(-EINVAL);
	return delete_grants(db,
		"DELETE FROM permission_grants WHERE project_root=?",
		project_root, NULL, 0, deleted);
}

int permission_grant_clear_all(struct db *db, int *deleted)
{
	return delete_grants(db, "DELETE FROM permission_grants",
			     NULL, NULL, 0, deleted);
}
