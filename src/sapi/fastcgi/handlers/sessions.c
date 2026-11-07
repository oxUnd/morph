/* sessions.c — session CRUD over fcgi_session_owner ⨝ sessions */
#define _GNU_SOURCE
#include "handlers.h"
#include "sapi/fastcgi/storage/sessions.h"
#include "../session_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/json.h"

/* POST /api/sessions
 * { "name": "...", "model": "..." }
 */
void handle_create_session(request_t *r)
{
	char *body = NULL;
	size_t blen = 0;
	{
		int rc = fcgi_read_body(r, &body, &blen);
		if (rc != 0) {
			reply_body_error(r, rc);
			return;
		}
	}

	struct json *root = json_parse_prefix(body ? body : "{}", NULL, NULL);
	const char *name = "untitled";
	const char *model = "default";
	if (json_is_object_value(root)) {
		struct json *n = json_member_ci(root, "name");
		struct json *m = json_member_ci(root, "model");
		if (json_is_string_value(n) && json_string_value(n))
			name = json_string_value(n);
		if (json_is_string_value(m) && json_string_value(m))
			model = json_string_value(m);
	}

	char sid[64] = {0};
	int rc = store_create_session(r->store, r->user_id, name, model, sid);
	json_free(root);
	free(body);

	if (rc != 0) {
		reply_500(r, "create failed");
		return;
	}

	char out[256];
	snprintf(out, sizeof(out), "{\"id\":\"%s\"}", sid);
	reply_201_json(r, out);
}

/* GET /api/sessions  → list owned by current user */
void handle_list_sessions(request_t *r)
{
	char *json = NULL;
	int limit = (int)query_i64(r, "limit", 50);
	int offset = (int)query_i64(r, "offset", 0);

	if (limit <= 0 || limit > 200)
		limit = 50;
	if (offset < 0)
		offset = 0;
	if (store_list_sessions_page_json(r->store, r->user_id, limit, offset, &json) != 0) {
		reply_500(r, "list failed");
		return;
	}
	reply_200_json(r, json);
	free(json);
}

/* GET /api/sessions/:id */
void handle_get_session(request_t *r)
{
	const char *sid = path_param(r, "id");
	if (!sid) {
		reply_400(r, "missing id");
		return;
	}
	if (!store_session_owned_by(r->store, sid, r->user_id)) {
		reply_403(r);
		return;
	}
	char buf[256];
	snprintf(buf, sizeof(buf), "{\"id\":\"%s\"}", sid);
	reply_200_json(r, buf);
}

/* PATCH /api/sessions/:id  { "name": "...", "model": "..." } */
void handle_update_session(request_t *r)
{
	const char *sid = path_param(r, "id");
	char *body = NULL;
	size_t blen = 0;
	struct json *root;
	const char *name = NULL;
	const char *model = NULL;
	int rc;

	if (!sid) {
		reply_400(r, "missing id");
		return;
	}
	rc = fcgi_read_body(r, &body, &blen);
	if (rc != 0) {
		reply_body_error(r, rc);
		return;
	}
	root = json_parse_prefix(body ? body : "{}", NULL, NULL);
	free(body);
	if (!json_is_object_value(root)) {
		json_free(root);
		reply_400(r, "invalid JSON body");
		return;
	}
	{
		struct json *n = json_member_ci(root, "name");
		struct json *m = json_member_ci(root, "model");

		if (n && !json_is_string_value(n)) {
			json_free(root);
			reply_400(r, "name must be a string");
			return;
		}
		if (m && !json_is_string_value(m)) {
			json_free(root);
			reply_400(r, "model must be a string");
			return;
		}
		name = json_string_value(n);
		model = json_string_value(m);
	}
	rc = store_update_session(r->store, sid, r->user_id, name, model);
	json_free(root);
	if (rc == -ENOENT) {
		reply_404(r);
		return;
	}
	if (rc == -EINVAL) {
		reply_400(r, "nothing to update");
		return;
	}
	if (rc != 0) {
		reply_500(r, "update failed");
		return;
	}
	{
		char buf[256];

		snprintf(buf, sizeof(buf), "{\"id\":\"%s\"}", sid);
		reply_200_json(r, buf);
	}
}

/* GET /api/sessions/:id/messages?limit=&before= */
void handle_list_messages(request_t *r)
{
	const char *sid = path_param(r, "id");
	char *json = NULL;
	int limit = (int)query_i64(r, "limit", 100);
	int64_t before = query_i64(r, "before", 0);
	int rc;

	if (!sid) {
		reply_400(r, "missing id");
		return;
	}
	rc = store_messages_json(r->store, sid, r->user_id, limit, before, &json);
	if (rc == -ENOENT) {
		reply_404(r);
		return;
	}
	if (rc != 0) {
		reply_500(r, "messages failed");
		return;
	}
	reply_200_json(r, json);
	free(json);
}

/* DELETE /api/sessions/:id */
void handle_delete_session(request_t *r)
{
	const char *sid = path_param(r, "id");
	if (!sid) {
		reply_400(r, "missing id");
		return;
	}
	if (!store_session_owned_by(r->store, sid, r->user_id)) {
		reply_403(r);
		return;
	}
	int rc = store_delete_session(r->store, sid, r->user_id);
	if (rc == -EAGAIN) {
		reply_json(r, 409, "{\"error\":\"turn_in_progress\"}");
		return;
	}
	if (rc != 0) {
		reply_500(r, "delete failed");
		return;
	}
	reply_204(r);
}
