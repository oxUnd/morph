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
#include "agent/react.h"
#include "agent/tokenizer.h"
#include "agent/memory.h"
#include "session.h"

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
	  int (*cb)(enum react_step_type step_type, const char *payload, void *u),
	  void *u);

__attribute__((weak)) void
react_context_destroy(struct react_context *ctx);

__attribute__((weak)) int
react_memory_options_for_session(struct memory_options *out);
/* -------------------------------------------------------- */

struct turn_job {
	struct session_store *store;
	char  session_id[64];
	char  user_id[64];
	char *input;
	char  last_tool[128];
};

static int bridge_cb(enum react_step_type step_type, const char *payload_json, void *u) {
	struct turn_job *j = (struct turn_job *)u;
	if (!payload_json)
		payload_json = "";

	switch (step_type) {
	case 0: /* REACT_STEP_THOUGHT */
		event_sink_thought(j->store, j->session_id, payload_json);
		return 0;

	case 1: { /* REACT_STEP_ACTION -> tool_call */
		/* skip status messages like "Executing img_gen..." / "img_gen completed" */
		const char *paren = strchr(payload_json, '(');
		if (!paren) break;

		size_t name_len = (size_t)(paren - payload_json);
		if (name_len < 1 || name_len > 120) break;

		char tool_name[128];
		snprintf(tool_name, sizeof(tool_name), "%.*s",
			 (int)name_len, payload_json);

		/* extract args between ( and the trailing ) */
		const char *args_start = paren + 1;
		const char *end = payload_json + strlen(payload_json);
		if (end > args_start && end[-1] == ')')
			end--;

		size_t args_len = (size_t)(end - args_start);
		if (args_len > 4096) args_len = 4096;

		char args_json[4100];
		snprintf(args_json, sizeof(args_json), "%.*s",
			 (int)args_len, args_start);

		snprintf(j->last_tool, sizeof(j->last_tool), "%s", tool_name);
		event_sink_tool_call(j->store, j->session_id,
				     tool_name, args_json);
		return 0;
	}

	case 2: /* REACT_STEP_OBSERVATION -> tool_result */
		event_sink_tool_result(j->store, j->session_id,
				       j->last_tool, payload_json);
		j->last_tool[0] = '\0';
		return 0;

	case 3: /* REACT_STEP_REFLECTION — piggyback on thought schema */
		event_sink_thought(j->store, j->session_id, payload_json);
		return 0;

	case 4: /* REACT_STEP_FINAL */
		event_sink_final(j->store, j->session_id, payload_json);
		return 0;

	default:
		events_publish(j->store, j->session_id, "step",
			       payload_json);
		return 0;
	}
	return 0;
}

static void *turn_thread(void *arg) {
	struct turn_job *j = (struct turn_job *)arg;
	struct memory_options mem_opts = {
		.enabled = 1,
		.hot_path_enabled = 1,
		.cold_path_enabled = 1,
		.llm_extract_enabled = 1,
		.max_facts = 6,
		.max_episodes = 4,
		.max_procedures = 4,
		.max_context_chars = 3000,
	};
	struct session sess = {0};

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

	if (react_memory_options_for_session)
		react_memory_options_for_session(&mem_opts);
	if (session_get_by_display_id(&j->store->db, j->session_id, &sess) == 0) {
		char *memory_ctx = memory_build_context(&j->store->db, sess.id,
							j->input, &mem_opts);
		react_set_memory_context(rctx, memory_ctx);
		free(memory_ctx);
	}

	events_publish(j->store, j->session_id, "turn_start",
		       "{\"phase\":\"begin\"}");
	react_run(rctx, j->input ? j->input : "", bridge_cb, j);

	if (sess.id > 0) {
		cJSON *arr = cJSON_CreateArray();
		struct react_step *cur = rctx->steps;
		while (cur) {
			cJSON *obj = cJSON_CreateObject();
			cJSON_AddStringToObject(obj, "type",
						react_step_type_name(cur->type));
			if (cur->content)
				cJSON_AddStringToObject(obj, "content", cur->content);
			if (cur->tool_name)
				cJSON_AddStringToObject(obj, "tool_name", cur->tool_name);
			if (cur->tool_args)
				cJSON_AddStringToObject(obj, "tool_args", cur->tool_args);
			if (cur->tool_call_id)
				cJSON_AddStringToObject(obj, "tool_call_id",
							cur->tool_call_id);
			cJSON_AddItemToArray(arr, obj);
			cur = cur->next;
		}
		{
			char *json = cJSON_PrintUnformatted(arr);
			int round_no = trace_get_next_round_no(&j->store->db, sess.id);
			int aborted = (rctx->state == REACT_STATE_ABORT) ? 1 : 0;
			trace_save(&j->store->db, sess.id, round_no,
				   json ? json : "[]", aborted);
			free(json);
		}
		cJSON_Delete(arr);

		if (j->input && *j->input) {
			int user_tokens = tokenizer_count(rctx->tokenizer, j->input);
			message_add(&j->store->db, sess.id, "user", j->input,
				    user_tokens);
			session_update_tokens(&j->store->db, sess.id, user_tokens);
		}
		if (rctx->final_answer && *rctx->final_answer) {
			int asst_tokens =
				tokenizer_count(rctx->tokenizer, rctx->final_answer);
			message_add(&j->store->db, sess.id, "assistant",
				    rctx->final_answer, asst_tokens);
			session_update_tokens(&j->store->db, sess.id, asst_tokens);
		}
		memory_consolidate_turn(&j->store->db, sess.id, j->input,
					rctx->final_answer, rctx->steps,
					rctx->state == REACT_STATE_DONE,
					&mem_opts);
	}
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
