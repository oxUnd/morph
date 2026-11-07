#include "sapi/fastcgi/storage/canvas.h"
#include "sapi/fastcgi/session_store_internal.h"
#include "util/json.h"
#include "util/error.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>

int canvas_list_json(struct session_store *s, const char *session_id, char **out_json)
{
	const char *sql = "SELECT id,parent_id,kind,x,y,w,h,content_ref,meta,updated_at "
			  "FROM fcgi_canvas_nodes WHERE session_id=? ORDER BY updated_at";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);

	struct json *root = json_object();
	struct json *nodes = root ? json_add_array(root, "nodes") : NULL;
	if (!root || !nodes) {
		json_free(root);
		sqlite3_finalize(st);
		return -ENOMEM;
	}
	int step_rc;

	while ((step_rc = sqlite3_step(st)) == SQLITE_ROW) {
		const char *id = (const char *)sqlite3_column_text(st, 0);
		const char *parent = (const char *)sqlite3_column_text(st, 1);
		const char *kind = (const char *)sqlite3_column_text(st, 2);
		double x = sqlite3_column_double(st, 3);
		double y = sqlite3_column_double(st, 4);
		double w = sqlite3_column_double(st, 5);
		double h = sqlite3_column_double(st, 6);
		const char *cref = (const char *)sqlite3_column_text(st, 7);
		const char *meta = (const char *)sqlite3_column_text(st, 8);
		int64_t up = sqlite3_column_int64(st, 9);
		struct json *item = json_object();

		if (!item) {
			json_free(root);
			sqlite3_finalize(st);
			return -ENOMEM;
		}
		json_add_string(item, "id", id ? id : "");
		if (parent)
			json_add_string(item, "parent", parent);
		else
			json_add_null(item, "parent");
		json_add_string(item, "kind", kind ? kind : "");
		json_add_number(item, "x", x);
		json_add_number(item, "y", y);
		json_add_number(item, "w", w);
		json_add_number(item, "h", h);
		if (cref)
			json_add_string(item, "content_ref", cref);
		else
			json_add_null(item, "content_ref");
		if (meta && *meta) {
			struct json *meta_obj = json_parse_prefix(meta, NULL, NULL);
			if (meta_obj) {
				int rc = json_object_add(item, "meta", meta_obj);
				if (rc) {
					json_free(meta_obj);
					json_free(item);
					json_free(root);
					sqlite3_finalize(st);
					return rc;
				}
			} else {
				json_add_null(item, "meta");
			}
		} else {
			json_add_null(item, "meta");
		}
		json_add_number(item, "updated_at", (double)up);
		int rc = json_array_add(nodes, item);
		if (rc) {
			json_free(item);
			json_free(root);
			sqlite3_finalize(st);
			return rc;
		}
	}
	sqlite3_finalize(st);
	if (step_rc != SQLITE_DONE) {
		json_free(root);
		return -EIO;
	}
	char *json = json_stringify(root);
	json_free(root);
	if (!json)
		return -ENOMEM;
	*out_json = json;
	return 0;
}

static void mk_node_id(char out[40])
{
	int64_t t = fcgi_store_now();
	snprintf(out, 40, "n_%llx_%x", (long long)t, (unsigned)(rand() & 0xffffff));
}

int canvas_add_node(struct session_store *s, const char *session_id, const char *node_json,
		    char out_node_id[40])
{
	mk_node_id(out_node_id);
	const char *sql =
		"INSERT INTO "
		"fcgi_canvas_nodes(id,session_id,kind,x,y,w,h,meta,created_at,updated_at) "
		"VALUES(?,?,'node',0,0,0,0,?,?,?)";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(st, 1, out_node_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, node_json ? node_json : "{}", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 4, fcgi_store_now());
	sqlite3_bind_int64(st, 5, fcgi_store_now());
	int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	return rc == SQLITE_DONE ? 0 : -EIO;
}

int canvas_patch_node(struct session_store *s, const char *session_id, const char *node_id,
		      const char *patch_json)
{
	const char *sql = "UPDATE fcgi_canvas_nodes SET meta=?, updated_at=? "
			  "WHERE id=? AND session_id=?";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(st, 1, patch_json ? patch_json : "{}", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 2, fcgi_store_now());
	sqlite3_bind_text(st, 3, node_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 4, session_id, -1, SQLITE_TRANSIENT);
	int rc = sqlite3_step(st);
	int changes = sqlite3_changes(s->db.handle);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return -EIO;
	return changes > 0 ? 0 : -ENOENT;
}
