/* actions.c — Web → Agent command queue
 *
 * POST /api/sessions/:id/actions
 *   { "type": "approve" | "reject" | "cancel" | "prompt", "payload": {...} }
 */
#define _GNU_SOURCE
#include "handlers.h"
#include "sapi/fastcgi/storage/sessions.h"
#include "sapi/fastcgi/storage/events.h"
#include "../session_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/json.h"

void handle_post_action(request_t *r)
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
	if (!json_is_object_value(root)) {
		json_free(root);
		free(body);
		reply_400(r, "invalid json");
		return;
	}
	struct json *type_j = json_member_ci(root, "type");
	struct json *payload_j = json_member_ci(root, "payload");
	const char *type = json_is_string_value(type_j) ? json_string_value(type_j) : NULL;
	if (!type || !*type) {
		json_free(root);
		free(body);
		reply_400(r, "missing type");
		return;
	}

	char *payload_txt = payload_j ? json_stringify(payload_j) : strdup("{}");
	int rc = actions_enqueue(r->store, sid, type, payload_txt);
	free(payload_txt);
	json_free(root);
	free(body);
	if (rc != 0) {
		reply_500(r, "enqueue failed");
		return;
	}
	reply_202_json(r, "{\"queued\":true}");
}
