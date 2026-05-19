/* actions.c — Web → Agent command queue
 *
 * POST /api/sessions/:id/actions
 *   { "type": "approve" | "reject" | "cancel" | "prompt", "payload": {...} }
 */
#define _GNU_SOURCE
#include "handlers.h"
#include "../session_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

void handle_post_action(request_t *r) {
	const char *sid = path_param(r, "id");
	if (!sid) { reply_400(r, "missing id"); return; }
	if (!store_session_owned_by(r->store, sid, r->user_id)) { reply_403(r); return; }

	char *body = NULL; size_t blen = 0;
	if (fcgi_read_body(r, &body, &blen) != 0) { reply_400(r, "body too large"); return; }

	cJSON *root = cJSON_Parse(body ? body : "{}");
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root); free(body);
		reply_400(r, "invalid json"); return;
	}
	cJSON *type_j    = cJSON_GetObjectItem(root, "type");
	cJSON *payload_j = cJSON_GetObjectItem(root, "payload");
	const char *type = cJSON_IsString(type_j) ? type_j->valuestring : NULL;
	if (!type || !*type) {
		cJSON_Delete(root); free(body);
		reply_400(r, "missing type"); return;
	}

	char *payload_txt = payload_j ? cJSON_PrintUnformatted(payload_j)
				      : strdup("{}");
	int rc = actions_enqueue(r->store, sid, type, payload_txt);
	free(payload_txt);
	cJSON_Delete(root);
	free(body);
	if (rc != 0) { reply_500(r, "enqueue failed"); return; }
	reply_202_json(r, "{\"queued\":true}");
}
