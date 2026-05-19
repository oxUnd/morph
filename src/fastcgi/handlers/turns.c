/* turns.c — POST /api/sessions/:id/turns
 *
 * Spawns a worker thread that drives one ReAct round.  The thread uses the
 * existing public `react_run(ctx, input, cb, user)` callback API to bridge
 * react_step events into the FastCGI event store.  Web clients see the
 * stream via the SSE endpoint.
 *
 * Deep integration (cancellation, action injection) is optional and gated
 * on the weak symbol `react_context_create_for_session` — see PATCHES.md.
 */
#define _GNU_SOURCE
#include "handlers.h"
#include "../session_store.h"
#include "../event_sink.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* ------------ optional deep hook (weak link) ------------ */
struct react_context;
__attribute__((weak)) struct react_context *
react_context_create_for_session(struct session_store *store,
				 const char *session_id,
				 const char *user_id);

__attribute__((weak)) int
react_run(struct react_context *ctx, const char *user_input,
	  void (*cb)(int step_type, const char *payload, void *u), void *u);

__attribute__((weak)) void
react_context_destroy(struct react_context *ctx);
/* -------------------------------------------------------- */

struct turn_job {
	struct session_store *store;
	char  session_id[64];
	char  user_id[64];
	char *input;
};

static void bridge_cb(int step_type, const char *payload_json, void *u) {
	struct turn_job *j = (struct turn_job *)u;
	const char *type = "thought";
	switch (step_type) {
	case 0: type = "thought";     break;
	case 1: type = "tool_call";   break;
	case 2: type = "tool_result"; break;
	case 3: type = "reflection";  break;
	case 4: type = "final";       break;
	default: type = "step";       break;
	}
	events_publish(j->store, j->session_id, type,
		       payload_json ? payload_json : "{}");
}

static void *turn_thread(void *arg) {
	struct turn_job *j = (struct turn_job *)arg;

	if (!react_context_create_for_session || !react_run) {
		events_publish(j->store, j->session_id, "error",
			"{\"message\":\"react integration not linked; "
			"see fastcgi/PATCHES.md §3\"}");
		goto out;
	}

	struct react_context *rctx =
		react_context_create_for_session(j->store, j->session_id, j->user_id);
	if (!rctx) {
		events_publish(j->store, j->session_id, "error",
			"{\"message\":\"react_context_create_for_session failed\"}");
		goto out;
	}

	events_publish(j->store, j->session_id, "turn_start",
		       "{\"phase\":\"begin\"}");
	react_run(rctx, j->input ? j->input : "", bridge_cb, j);
	events_publish(j->store, j->session_id, "turn_end",
		       "{\"phase\":\"done\"}");

	if (react_context_destroy) react_context_destroy(rctx);
out:
	free(j->input);
	free(j);
	return NULL;
}

void handle_post_turn(request_t *r) {
	const char *sid = path_param(r, "id");
	if (!sid) { reply_400(r, "missing id"); return; }
	if (!store_session_owned_by(r->store, sid, r->user_id)) { reply_403(r); return; }

	char *body = NULL; size_t blen = 0;
	if (fcgi_read_body(r, &body, &blen) != 0) { reply_400(r, "body too large"); return; }
	cJSON *root = cJSON_Parse(body ? body : "{}");
	const char *input = "";
	if (cJSON_IsObject(root)) {
		cJSON *p = cJSON_GetObjectItem(root, "prompt");
		if (cJSON_IsString(p) && p->valuestring) input = p->valuestring;
	}

	struct turn_job *j = calloc(1, sizeof(*j));
	if (!j) { cJSON_Delete(root); free(body); reply_500(r, "oom"); return; }
	j->store = r->store;
	snprintf(j->session_id, sizeof(j->session_id), "%s", sid);
	snprintf(j->user_id,    sizeof(j->user_id),    "%s", r->user_id);
	j->input = strdup(input);

	pthread_t tid;
	if (pthread_create(&tid, NULL, turn_thread, j) != 0) {
		free(j->input); free(j); cJSON_Delete(root); free(body);
		reply_500(r, "thread spawn"); return;
	}
	pthread_detach(tid);

	cJSON_Delete(root);
	free(body);
	reply_202_json(r, "{\"accepted\":true}");
}
