/* sessions.c — session CRUD over fcgi_session_owner ⨝ sessions */
#define _GNU_SOURCE
#include "handlers.h"
#include "../session_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* POST /api/sessions
 * { "name": "...", "model": "..." }
 */
void handle_create_session(request_t *r) {
	char *body = NULL; size_t blen = 0;
	if (fcgi_read_body(r, &body, &blen) != 0) { reply_400(r, "body too large"); return; }

	cJSON *root = cJSON_Parse(body ? body : "{}");
	const char *name  = "untitled";
	const char *model = "default";
	if (cJSON_IsObject(root)) {
		cJSON *n = cJSON_GetObjectItem(root, "name");
		cJSON *m = cJSON_GetObjectItem(root, "model");
		if (cJSON_IsString(n) && n->valuestring) name = n->valuestring;
		if (cJSON_IsString(m) && m->valuestring) model = m->valuestring;
	}

	char sid[64] = {0};
	int rc = store_create_session(r->store, r->user_id, name, model, sid);
	cJSON_Delete(root);
	free(body);

	if (rc != 0) { reply_500(r, "create failed"); return; }

	char out[256];
	snprintf(out, sizeof(out), "{\"id\":\"%s\"}", sid);
	reply_201_json(r, out);
}

/* GET /api/sessions  → list owned by current user */
void handle_list_sessions(request_t *r) {
	char *json = NULL;
	if (store_list_sessions_json(r->store, r->user_id, &json) != 0) {
		reply_500(r, "list failed"); return;
	}
	reply_200_json(r, json);
	free(json);
}

/* GET /api/sessions/:id */
void handle_get_session(request_t *r) {
	const char *sid = path_param(r, "id");
	if (!sid) { reply_400(r, "missing id"); return; }
	if (!store_session_owned_by(r->store, sid, r->user_id)) { reply_403(r); return; }
	char buf[256];
	snprintf(buf, sizeof(buf), "{\"id\":\"%s\"}", sid);
	reply_200_json(r, buf);
}

/* DELETE /api/sessions/:id  (MVP: stub — leave row, just ack) */
void handle_delete_session(request_t *r) {
	const char *sid = path_param(r, "id");
	if (!sid) { reply_400(r, "missing id"); return; }
	if (!store_session_owned_by(r->store, sid, r->user_id)) { reply_403(r); return; }
	reply_204(r);
}
