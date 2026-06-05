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
#include "util/file.h"

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
	struct turn_artifact {
		char path[PATH_MAX];
		char id[64];
		char kind[16];
		char mime[64];
		char filename[128];
		char url[128];
		int64_t size_bytes;
	} artifacts[32];
	int artifacts_count;
};

static const char *bridge_output_dir(void)
{
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

static const char *artifact_kind_for_path(const char *path)
{
	const char *dot;

	if (!path)
		return NULL;
	dot = strrchr(path, '.');
	if (!dot)
		return NULL;
	if (strcmp(dot, ".png") == 0 || strcmp(dot, ".jpg") == 0 ||
	    strcmp(dot, ".jpeg") == 0 || strcmp(dot, ".webp") == 0)
		return "image";
	if (strcmp(dot, ".mp4") == 0 || strcmp(dot, ".webm") == 0 ||
	    strcmp(dot, ".mov") == 0 || strcmp(dot, ".mkv") == 0)
		return "video";
	return NULL;
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

static void artifact_publish_ready(struct turn_job *j,
				   const struct turn_artifact *a)
{
	cJSON *obj;
	char *json;

	obj = cJSON_CreateObject();
	if (!obj)
		return;
	cJSON_AddStringToObject(obj, "id", a->id);
	cJSON_AddStringToObject(obj, "kind", a->kind);
	cJSON_AddStringToObject(obj, "mime", a->mime);
	cJSON_AddStringToObject(obj, "filename", a->filename);
	cJSON_AddNumberToObject(obj, "size_bytes", (double)a->size_bytes);
	cJSON_AddStringToObject(obj, "url", a->url);
	json = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	if (!json)
		return;
	events_publish(j->store, j->session_id, "artifact_ready", json);
	free(json);
}

static struct turn_artifact *turn_artifact_get(struct turn_job *j,
					       const char *path,
					       const char *kind)
{
	char rel[512];
	char real[PATH_MAX];
	char *expanded;
	struct stat st;
	const char *filename;
	struct turn_artifact *a;

	if (!j || !path || !kind)
		return NULL;
	expanded = file_expand_path(path);
	if (!expanded)
		return NULL;
	if (stat(expanded, &st) != 0 || !S_ISREG(st.st_mode)) {
		free(expanded);
		return NULL;
	}
	if (!realpath(expanded, real)) {
		free(expanded);
		return NULL;
	}
	free(expanded);
	if (!relative_under_root(bridge_output_dir(), real, rel))
		return NULL;
	for (int i = 0; i < j->artifacts_count; i++) {
		if (strcmp(j->artifacts[i].path, real) == 0)
			return &j->artifacts[i];
	}
	if (j->artifacts_count >= (int)(sizeof(j->artifacts) /
				       sizeof(j->artifacts[0])))
		return NULL;

	filename = strrchr(real, '/');
	filename = filename ? filename + 1 : real;
	a = &j->artifacts[j->artifacts_count];
	memset(a, 0, sizeof(*a));
	snprintf(a->path, sizeof(a->path), "%s", real);
	snprintf(a->kind, sizeof(a->kind), "%s", kind);
	snprintf(a->mime, sizeof(a->mime), "%s", artifact_mime(kind, real));
	snprintf(a->filename, sizeof(a->filename), "%s", filename);
	a->size_bytes = st.st_size;
	if (store_artifact_register(j->store, j->user_id, j->session_id,
				    a->kind, a->mime, a->filename, rel,
				    st.st_size, a->id) != 0) {
		memset(a, 0, sizeof(*a));
		return NULL;
	}
	snprintf(a->url, sizeof(a->url), "/api/artifacts/%s", a->id);
	j->artifacts_count++;
	artifact_publish_ready(j, a);
	return a;
}

static int artifact_from_generated_text(const char *payload, char path[PATH_MAX],
					const char **kind_out)
{
	const char *prefix = NULL;
	const char *kind = NULL;
	const char *start;
	const char *end;

	if (!payload)
		return 0;
	if (strncmp(payload, "image generated:", 16) == 0) {
		prefix = "image generated:";
		kind = "image";
	} else if (strncmp(payload, "video generated:", 16) == 0) {
		prefix = "video generated:";
		kind = "video";
	} else {
		return 0;
	}

	start = payload + strlen(prefix);
	while (*start == ' ' || *start == '\t')
		start++;
	end = strstr(start, " (");
	if (!end)
		end = start + strlen(start);
	while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	if (end <= start || (size_t)(end - start) >= PATH_MAX)
		return 0;
	snprintf(path, PATH_MAX, "%.*s", (int)(end - start), start);
	if (kind_out)
		*kind_out = kind;
	return 1;
}

static void maybe_publish_artifact(struct turn_job *j, const char *payload)
{
	char path[PATH_MAX];
	const char *kind = NULL;

	if (!artifact_from_generated_text(payload, path, &kind))
		return;
	(void)turn_artifact_get(j, path, kind);
}

static int path_char(int c)
{
	return c > 32 && c != '"' && c != '\'' && c != '<' && c != '>' &&
		c != '[' && c != ']' && c != '(' && c != ')' &&
		c != '{' && c != '}';
}

static void trim_candidate_path(char *path)
{
	size_t len;

	len = strlen(path);
	while (len > 0 && (path[len - 1] == ')' || path[len - 1] == ',' ||
			   path[len - 1] == '.' || path[len - 1] == ';' ||
			   path[len - 1] == ':' || path[len - 1] == '!')) {
		path[len - 1] = '\0';
		len--;
	}
}

static int append_str(char **buf, size_t *len, size_t *cap, const char *s)
{
	size_t slen;
	char *tmp;

	if (!buf || !len || !cap || !s)
		return -EINVAL;
	slen = strlen(s);
	if (*len + slen + 1 > *cap) {
		size_t ncap = *cap ? *cap : 256;
		while (*len + slen + 1 > ncap)
			ncap *= 2;
		tmp = realloc(*buf, ncap);
		if (!tmp)
			return -ENOMEM;
		*buf = tmp;
		*cap = ncap;
	}
	memcpy(*buf + *len, s, slen);
	*len += slen;
	(*buf)[*len] = '\0';
	return 0;
}

static int append_mem(char **buf, size_t *len, size_t *cap,
		      const char *s, size_t slen)
{
	char *tmp;

	if (!buf || !len || !cap || (!s && slen > 0))
		return -EINVAL;
	if (*len + slen + 1 > *cap) {
		size_t ncap = *cap ? *cap : 256;
		while (*len + slen + 1 > ncap)
			ncap *= 2;
		tmp = realloc(*buf, ncap);
		if (!tmp)
			return -ENOMEM;
		*buf = tmp;
		*cap = ncap;
	}
	if (slen > 0)
		memcpy(*buf + *len, s, slen);
	*len += slen;
	(*buf)[*len] = '\0';
	return 0;
}

static void html_attr_escape(const char *src, char *dst, size_t dst_size)
{
	size_t pos = 0;

	if (!dst || dst_size == 0)
		return;
	dst[0] = '\0';
	if (!src)
		return;
	for (const char *p = src; *p && pos + 1 < dst_size; p++) {
		const char *rep = NULL;
		switch (*p) {
		case '&':
			rep = "&amp;";
			break;
		case '"':
			rep = "&quot;";
			break;
		case '<':
			rep = "&lt;";
			break;
		case '>':
			rep = "&gt;";
			break;
		default:
			break;
		}
		if (rep) {
			size_t rlen = strlen(rep);
			if (pos + rlen >= dst_size)
				break;
			memcpy(dst + pos, rep, rlen);
			pos += rlen;
		} else {
			dst[pos++] = *p;
		}
	}
	dst[pos] = '\0';
}

static int append_artifact_tag(char **buf, size_t *len, size_t *cap,
			       const struct turn_artifact *a,
			       const char *session_id)
{
	char tag[1400];
	char filename[512];
	char url[256];
	char gallery_id[64];

	if (!a)
		return -EINVAL;
	html_attr_escape(a->filename, filename, sizeof(filename));
	html_attr_escape(a->url, url, sizeof(url));
	if (strcmp(a->kind, "image") == 0) {
		snprintf(gallery_id, sizeof(gallery_id), "s%s",
			 session_id ? session_id : "0");
		snprintf(tag, sizeof(tag),
			 "<a class=\"glightbox\" data-gallery=\"%s\""
			 " href=\"%s\">"
			 "<img data-src=\"%s\" alt=\"%s\" loading=\"lazy\">"
			 "</a>",
			 gallery_id, url, url, filename);
	} else {
		snprintf(tag, sizeof(tag),
			 "<a class=\"glightbox\" data-type=\"video\""
			 " href=\"%s\">"
			 "<video data-src=\"%s\" controls preload=\"metadata\""
			 " style=\"pointer-events:none\">"
			 "</video></a>",
			 url, url);
	}
	return append_str(buf, len, cap, tag);
}

static int candidate_path(const char *token, char out[PATH_MAX])
{
	if (!token || !*token)
		return 0;
	if (token[0] == '/' || token[0] == '~') {
		snprintf(out, PATH_MAX, "%s", token);
		return 1;
	}
	snprintf(out, PATH_MAX, "%s/%s", bridge_output_dir(), token);
	return 1;
}

static char *render_media_refs(struct turn_job *j, const char *text)
{
	const char *p;
	char *out = NULL;
	size_t len = 0;
	size_t cap = 0;
	char generated_path[PATH_MAX];
	const char *generated_kind = NULL;

	if (!text)
		return strdup("");
	if (artifact_from_generated_text(text, generated_path,
					 &generated_kind)) {
		struct turn_artifact *a =
			turn_artifact_get(j, generated_path, generated_kind);
		if (a) {
			const char *start = strstr(text, generated_path);
			if (start) {
				append_mem(&out, &len, &cap, text,
					   (size_t)(start - text));
				append_artifact_tag(&out, &len, &cap, a,
						    j->session_id);
				append_str(&out, &len, &cap,
					   start + strlen(generated_path));
				return out ? out : strdup(text);
			}
		}
	}

	p = text;
	while (*p) {
		const char *start = p;
		char token[PATH_MAX];
		char path[PATH_MAX];
		const char *kind;
		struct turn_artifact *a;
		size_t raw_len;
		size_t tok_len;

		while (*p && !path_char((unsigned char)*p))
			p++;
		if (p > start)
			append_mem(&out, &len, &cap, start, (size_t)(p - start));
		start = p;
		while (*p && path_char((unsigned char)*p))
			p++;
		raw_len = (size_t)(p - start);
		if (raw_len == 0)
			continue;
		tok_len = raw_len < sizeof(token) ? raw_len : sizeof(token) - 1;
		snprintf(token, sizeof(token), "%.*s", (int)tok_len, start);
		trim_candidate_path(token);
		kind = artifact_kind_for_path(token);
		if (!kind || !candidate_path(token, path)) {
			append_mem(&out, &len, &cap, start, raw_len);
			continue;
		}
		a = turn_artifact_get(j, path, kind);
		if (!a) {
			append_mem(&out, &len, &cap, start, raw_len);
			continue;
		}
		append_artifact_tag(&out, &len, &cap, a, j->session_id);
		if (strlen(token) < raw_len)
			append_mem(&out, &len, &cap, start + strlen(token),
				   raw_len - strlen(token));
	}
	return out ? out : strdup(text);
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
	{
		char *rendered = render_media_refs(j, payload_json);
		event_sink_final(j->store, j->session_id,
				 rendered ? rendered : payload_json);
		free(rendered);
		return 0;
	}

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
			char *rendered =
				render_media_refs(j, rctx->final_answer);
			const char *answer =
				rendered ? rendered : rctx->final_answer;
			int asst_tokens =
				tokenizer_count(rctx->tokenizer, answer);
			message_add(&j->store->db, sess.id, "assistant", answer,
				    asst_tokens);
			session_update_tokens(&j->store->db, sess.id, asst_tokens);
			free(rendered);
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
