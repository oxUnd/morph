/* canvas.c — canvas node CRUD */
#define _GNU_SOURCE
#include "handlers.h"
#include "../session_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void handle_get_canvas(request_t *r) {
	const char *sid = path_param(r, "id");
	if (!sid) { reply_400(r, "missing id"); return; }
	if (!store_session_owned_by(r->store, sid, r->user_id)) { reply_403(r); return; }

	char *json = NULL;
	if (canvas_list_json(r->store, sid, &json) != 0) {
		reply_500(r, "list failed"); return;
	}
	reply_200_json(r, json);
	free(json);
}

void handle_add_canvas_node(request_t *r) {
	const char *sid = path_param(r, "id");
	if (!sid) { reply_400(r, "missing id"); return; }
	if (!store_session_owned_by(r->store, sid, r->user_id)) { reply_403(r); return; }

	char *body = NULL; size_t blen = 0;
	if (fcgi_read_body(r, &body, &blen) != 0) { reply_400(r, "body too large"); return; }

	char node_id[40] = {0};
	int rc = canvas_add_node(r->store, sid, body, node_id);
	if (rc != 0) { free(body); reply_500(r, "add node"); return; }

	/* notify subscribers */
	char ev[128];
	snprintf(ev, sizeof(ev), "{\"node_id\":\"%s\"}", node_id);
	events_publish(r->store, sid, "canvas_node_added", ev);

	char out[128];
	snprintf(out, sizeof(out), "{\"id\":\"%s\"}", node_id);
	free(body);
	reply_201_json(r, out);
}

void handle_patch_canvas_node(request_t *r) {
	const char *sid  = path_param(r, "id");
	const char *node = path_param(r, "node");
	if (!sid || !node) { reply_400(r, "missing id"); return; }
	if (!store_session_owned_by(r->store, sid, r->user_id)) { reply_403(r); return; }

	char *body = NULL; size_t blen = 0;
	if (fcgi_read_body(r, &body, &blen) != 0) { reply_400(r, "body too large"); return; }

	int rc = canvas_patch_node(r->store, sid, node, body);
	free(body);
	if (rc != 0) { reply_404(r); return; }

	char ev[128];
	snprintf(ev, sizeof(ev), "{\"node_id\":\"%s\"}", node);
	events_publish(r->store, sid, "canvas_node_patched", ev);

	reply_204(r);
}
