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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

__attribute__((weak)) const char *fcgi_artifact_output_dir(void);
/* -------------------------------------------------------- */

struct turn_job {
	struct session_store *store;
	char  session_id[64];
	char  user_id[64];
	char  turn_id[64];
	char *input;
	char  last_tool[128];
};

static const char *bridge_output_dir(void)
{
	const char *env = getenv("MORPH_FCGI_OUTPUT_DIR");

	if (env && *env)
		return env;
	if (fcgi_artifact_output_dir)
		return fcgi_artifact_output_dir();
	return "/var/lib/morph/output";
}

static const char *artifact_mime(const char *kind, const char *path)
{
	const char *dot = strrchr(path, '.');

	if (strcmp(kind, "image") == 0) {
		if (dot && strcmp(dot, ".jpg") == 0)
			return "image/jpeg";
		if (dot && strcmp(dot, ".jpeg") == 0)
			return "image/jpeg";
		if (dot && strcmp(dot, ".webp") == 0)
			return "image/webp";
		return "image/png";
	}
	if (dot && strcmp(dot, ".webm") == 0)
		return "video/webm";
	return "video/mp4";
}

static int relative_under_root(const char *root, const char *path,
			       char rel[512])
{
	char root_real[PATH_MAX];
	char file_real[PATH_MAX];
	size_t root_len;

	if (!realpath(root, root_real))
		return 0;
	if (!realpath(path, file_real))
		return 0;
	root_len = strlen(root_real);
	if (strncmp(root_real, file_real, root_len) != 0)
		return 0;
	if (file_real[root_len] != '/' && file_real[root_len] != '\0')
		return 0;
	snprintf(rel, 512, "%s",
		 file_real[root_len] == '/' ? file_real + root_len + 1 : "");
	return rel[0] != '\0';
}

static void maybe_publish_artifact(struct turn_job *j, const char *payload)
{
	const char *prefix = NULL;
	const char *kind = NULL;
	const char *start;
	const char *end;
	char path[PATH_MAX];
	char rel[512];
	char artifact_id[64];
	const char *filename;
	struct stat st;
	const char *mime;
	char url[128];
	cJSON *obj;
	char *json;

	if (!payload)
		return;
	if (strncmp(payload, "image generated:", 16) == 0) {
		prefix = "image generated:";
		kind = "image";
	} else if (strncmp(payload, "video generated:", 16) == 0) {
		prefix = "video generated:";
		kind = "video";
	} else {
		return;
	}

	start = payload + strlen(prefix);
	while (*start == ' ' || *start == '\t')
		start++;
	end = strstr(start, " (");
	if (!end)
		end = start + strlen(start);
	while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	if (end <= start || (size_t)(end - start) >= sizeof(path))
		return;
	snprintf(path, sizeof(path), "%.*s", (int)(end - start), start);
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return;
	if (!relative_under_root(bridge_output_dir(), path, rel))
		return;

	filename = strrchr(path, '/');
	filename = filename ? filename + 1 : path;
	mime = artifact_mime(kind, path);
	if (store_artifact_register(j->store, j->user_id, j->session_id,
				    kind, mime, filename, rel, st.st_size,
				    artifact_id) != 0) {
		return;
	}

	obj = cJSON_CreateObject();
	if (!obj)
		return;
	cJSON_AddStringToObject(obj, "id", artifact_id);
	cJSON_AddStringToObject(obj, "kind", kind);
	cJSON_AddStringToObject(obj, "mime", mime);
	cJSON_AddStringToObject(obj, "filename", filename);
	cJSON_AddNumberToObject(obj, "size_bytes", (double)st.st_size);
	snprintf(url, sizeof(url), "/api/artifacts/%s", artifact_id);
	cJSON_AddStringToObject(obj, "url", url);
	json = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	if (!json)
		return;
	events_publish(j->store, j->session_id, "artifact_ready", json);
	free(json);
}

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
		maybe_publish_artifact(j, payload_json);
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

	{
		cJSON *start = cJSON_CreateObject();
		char *json = NULL;
		if (start) {
			cJSON_AddStringToObject(start, "phase", "begin");
			cJSON_AddStringToObject(start, "turn_id", j->turn_id);
			json = cJSON_PrintUnformatted(start);
			cJSON_Delete(start);
		}
		events_publish(j->store, j->session_id, "turn_start",
			       json ? json : "{\"phase\":\"begin\"}");
		free(json);
	}
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
		memory_consolidate_turn_async(&j->store->db, sess.id,
					      j->input,
					      rctx->final_answer, rctx->steps,
					      rctx->state == REACT_STATE_DONE,
					      &mem_opts);
	}
	events_publish(j->store, j->session_id, "turn_end",
		       "{\"phase\":\"done\"}");

	if (react_context_destroy) react_context_destroy(rctx);
out:
	store_quota_end_turn(j->store, j->turn_id);
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
	{
		int qrc = store_quota_begin_turn(r->store, r->user_id, sid,
						 j->turn_id);
		if (qrc == -EDQUOT) {
			free(j); cJSON_Delete(root); free(body);
			reply_json(r, 429,
				"{\"error\":\"quota_exceeded\","
				"\"resource\":\"turns\"}");
			return;
		}
		if (qrc == -EAGAIN) {
			free(j); cJSON_Delete(root); free(body);
			reply_json(r, 429,
				"{\"error\":\"quota_exceeded\","
				"\"resource\":\"concurrent_turns\"}");
			return;
		}
		if (qrc != 0) {
			free(j); cJSON_Delete(root); free(body);
			reply_500(r, "quota failed"); return;
		}
	}
	j->input = strdup(input);
	if (!j->input) {
		store_quota_end_turn(r->store, j->turn_id);
		free(j); cJSON_Delete(root); free(body);
		reply_500(r, "oom"); return;
	}

	pthread_t tid;
	if (pthread_create(&tid, NULL, turn_thread, j) != 0) {
		store_quota_end_turn(r->store, j->turn_id);
		free(j->input); free(j); cJSON_Delete(root); free(body);
		reply_500(r, "thread spawn"); return;
	}
	pthread_detach(tid);

	cJSON_Delete(root);
	free(body);
	reply_202_json(r, "{\"accepted\":true}");
}
