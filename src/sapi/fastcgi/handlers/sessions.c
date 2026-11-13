/* sessions.c — session CRUD over fcgi_session_owner ⨝ sessions */
#define _GNU_SOURCE
#include "handlers.h"
#include "nb/err.h"
#include "sapi/fastcgi/storage/sessions.h"
#include "../session_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

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

	cJSON *root = cJSON_Parse(body ? body : "{}");
	const char *name = "untitled";
	const char *model = "default";
	if (cJSON_IsObject(root)) {
		cJSON *n = cJSON_GetObjectItem(root, "name");
		cJSON *m = cJSON_GetObjectItem(root, "model");
		if (cJSON_IsString(n) && n->valuestring)
			name = n->valuestring;
		if (cJSON_IsString(m) && m->valuestring)
			model = m->valuestring;
	}

	char sid[64] = {0};
	int rc = store_create_session(r->store, r->user_id, name, model, sid);
	cJSON_Delete(root);
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
	const char *sid = path_param_borrow(r, "id");
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
	const char *sid = path_param_borrow(r, "id");
	char *body = NULL;
	size_t blen = 0;
	cJSON *root;
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
	root = cJSON_Parse(body ? body : "{}");
	free(body);
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		reply_400(r, "invalid JSON body");
		return;
	}
	{
		cJSON *n = cJSON_GetObjectItem(root, "name");
		cJSON *m = cJSON_GetObjectItem(root, "model");

		if (n && !cJSON_IsString(n)) {
			cJSON_Delete(root);
			reply_400(r, "name must be a string");
			return;
		}
		if (m && !cJSON_IsString(m)) {
			cJSON_Delete(root);
			reply_400(r, "model must be a string");
			return;
		}
		name = cJSON_GetStringValue(n);
		model = cJSON_GetStringValue(m);
	}
	rc = store_update_session(r->store, sid, r->user_id, name, model);
	cJSON_Delete(root);
	if (rc == NB_ERR_NOT_FOUND) {
		reply_404(r);
		return;
	}
	if (rc == NB_ERR_INVAL) {
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
	const char *sid = path_param_borrow(r, "id");
	char *json = NULL;
	int limit = (int)query_i64(r, "limit", 100);
	int64_t before = query_i64(r, "before", 0);
	int rc;

	if (!sid) {
		reply_400(r, "missing id");
		return;
	}
	rc = store_messages_json(r->store, sid, r->user_id, limit, before, &json);
	if (rc == NB_ERR_NOT_FOUND) {
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
	const char *sid = path_param_borrow(r, "id");
	if (!sid) {
		reply_400(r, "missing id");
		return;
	}
	if (!store_session_owned_by(r->store, sid, r->user_id)) {
		reply_403(r);
		return;
	}
	int rc = store_delete_session(r->store, sid, r->user_id);
	if (rc == NB_ERR_AGAIN) {
		reply_json(r, 409, "{\"error\":\"turn_in_progress\"}");
		return;
	}
	if (rc != 0) {
		reply_500(r, "delete failed");
		return;
	}
	reply_204(r);
}
