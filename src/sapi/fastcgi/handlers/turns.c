/* turns.c — POST /api/sessions/:id/turns
 *
 * Spawns a worker thread that drives one ReAct round through the shared
 * runtime layer and bridges ReAct events into
 * the FastCGI event store.  Web clients see the stream via the SSE endpoint.
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
#include "agent/turn.h"
#include "event/event.h"
#include "runtime/runtime.h"
#include "models/llm.h"
#include "config.h"
#include "credits.h"
#include "session.h"
#include "util/error.h"
#include "util/file.h"

#include <pthread.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util/buf.h"

#include "cJSON.h"

/* ------------ optional deep hook (weak link) ------------ */
struct react_context;
__attribute__((weak)) struct react_context *
react_context_create_for_session(struct session_store *store,
				 const char *session_id,
				 const char *user_id);

__attribute__((weak)) void
react_context_destroy(struct react_context *ctx);

__attribute__((weak)) int
react_set_event_callback(struct react_context *ctx,
			 morph_event_cb cb, void *user);

__attribute__((weak)) int
react_set_turn_id(struct react_context *ctx, const char *turn_id);

__attribute__((weak)) int
react_memory_options_for_session(struct memory_options *out);

__attribute__((weak)) const char *fcgi_artifact_output_dir(void);
__attribute__((weak)) const struct config *fcgi_bridge_config(void);
/* -------------------------------------------------------- */

struct turn_job {
	struct session_store *store;
	char  session_id[64];
	char  user_id[64];
	char  turn_id[64];
	char *input;
	char  last_tool[128];
	char  last_tool_call_id[128];
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
	int final_sent;
};

static __thread struct turn_job *current_usage_job;

static void publish_credit_warning(struct turn_job *j,
				   const struct config *cfg)
{
	struct credit_summary today;
	cJSON *obj = NULL;
	char *json = NULL;

	if (!j || !cfg || cfg->credits.daily_limit < 0)
		return;
	if (credit_summary_today(&j->store->db, j->user_id, &today) != 0)
		return;
	if (today.credits <= cfg->credits.daily_limit)
		return;

	obj = cJSON_CreateObject();
	if (!obj)
		return;
	cJSON_AddNumberToObject(obj, "used_today", (double)today.credits);
	cJSON_AddNumberToObject(obj, "daily_limit",
				cfg->credits.daily_limit);
	cJSON_AddStringToObject(obj, "currency",
				cfg->credits.currency);
	cJSON_AddNumberToObject(obj, "estimated_cost_today",
				today.estimated_cost);
	cJSON_AddStringToObject(obj, "turn_id", j->turn_id);
	json = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	events_publish(j->store, j->session_id, "credits_warning",
		       json ? json : "{}");
	free(json);
}

static char *model_usage_metadata_json(const struct model_usage *usage)
{
	cJSON *obj;
	char *json;

	if (!usage)
		return NULL;
	obj = cJSON_CreateObject();
	if (!obj)
		return NULL;
	if (usage->response_id[0])
		cJSON_AddStringToObject(obj, "response_id",
					usage->response_id);
	if (usage->model[0])
		cJSON_AddStringToObject(obj, "actual_model", usage->model);
	if (usage->finish_reason[0])
		cJSON_AddStringToObject(obj, "finish_reason",
					usage->finish_reason);
	if (usage->system_fingerprint[0])
		cJSON_AddStringToObject(obj, "system_fingerprint",
					usage->system_fingerprint);
	if (usage->usage_source[0])
		cJSON_AddStringToObject(obj, "usage_source",
					usage->usage_source);
	if (usage->created > 0)
		cJSON_AddNumberToObject(obj, "created",
					(double)usage->created);
	if (usage->total_tokens > 0)
		cJSON_AddNumberToObject(obj, "total_tokens",
					(double)usage->total_tokens);
	if (usage->cached_tokens > 0)
		cJSON_AddNumberToObject(obj, "cached_tokens",
					(double)usage->cached_tokens);
	if (usage->reasoning_tokens > 0)
		cJSON_AddNumberToObject(obj, "reasoning_tokens",
					(double)usage->reasoning_tokens);
	if (usage->audio_tokens > 0)
		cJSON_AddNumberToObject(obj, "audio_tokens",
					(double)usage->audio_tokens);
	if (usage->image_tokens > 0)
		cJSON_AddNumberToObject(obj, "image_tokens",
					(double)usage->image_tokens);
	json = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	return json;
}

static void record_model_usage(const struct model_usage *usage,
			       void *user_data)
{
	struct turn_job *j = user_data ? user_data : current_usage_job;
	const struct config *cfg;
	struct credit_event event;
	char *metadata;

	if (!j || !usage || !fcgi_bridge_config)
		return;
	if (usage->input_tokens <= 0 && usage->output_tokens <= 0 &&
	    usage->image_units <= 0 && usage->video_seconds <= 0)
		return;
	cfg = fcgi_bridge_config();
	if (!cfg)
		return;
	metadata = model_usage_metadata_json(usage);
	memset(&event, 0, sizeof(event));
	event.user_id = j->user_id;
	event.session_id = j->session_id;
	event.kind = usage->kind[0] ? usage->kind : "model_text";
	event.provider = usage->provider[0] ? usage->provider :
		cfg->models.text.provider;
	event.model = usage->model[0] ? usage->model :
		cfg->models.text.model;
	event.input_tokens = usage->input_tokens;
	event.output_tokens = usage->output_tokens;
	event.image_units = usage->image_units;
	event.video_seconds = usage->video_seconds;
	event.metadata_json = metadata;
	if (credit_record_event(&j->store->db, &cfg->credits,
				&event, NULL) == 0)
		publish_credit_warning(j, cfg);
	free(metadata);
}

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
	cJSON_AddStringToObject(obj, "turn_id", j->turn_id);
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
	char artifact_id[64];

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
	snprintf(a->filename, sizeof(a->filename), "%.*s",
		 (int)sizeof(a->filename) - 1, filename);
	a->size_bytes = st.st_size;
	if (store_artifact_register(j->store, j->user_id, j->session_id,
				    a->kind, a->mime, a->filename, rel,
				    st.st_size, artifact_id) != 0) {
		memset(a, 0, sizeof(*a));
		return NULL;
	}
	snprintf(a->id, sizeof(a->id), "%s", artifact_id);
	snprintf(a->url, sizeof(a->url), "/api/artifacts/%s", artifact_id);
	j->artifacts_count++;
	artifact_publish_ready(j, a);
	return a;
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

static int append_artifact_tag(morph_buf_t *buf, const struct turn_artifact *a)
{
	char tag[1400];
	char filename[512];
	char url[256];

	if (!a)
		return -EINVAL;
	html_attr_escape(a->filename, filename, sizeof(filename));
	html_attr_escape(a->url, url, sizeof(url));
	if (strcmp(a->kind, "image") == 0) {
		snprintf(tag, sizeof(tag),
			 "<a href=\"%s\">"
			 "<img data-src=\"%s\" alt=\"%s\" loading=\"lazy\">"
			 "</a>",
			 url, url, filename);
	} else {
		snprintf(tag, sizeof(tag),
			 "<a href=\"%s\">"
			 "<video data-src=\"%s\" controls preload=\"metadata\""
			 "></video></a>",
			 url, url);
	}
	return morph_buf_puts(buf, tag);
}

static int candidate_path(const char *token, char out[PATH_MAX])
{
	if (!token || !*token)
		return 0;
	if (token[0] == '/' || token[0] == '~') {
		snprintf(out, PATH_MAX, "%s", token);
		return 1;
	}
	if (file_path_join(out, PATH_MAX, bridge_output_dir(), token) != 0)
		return 0;
	return 1;
}

static char *render_media_refs(struct turn_job *j, const char *text)
{
	const char *p;
	morph_buf_t out;

	if (!text)
		return strdup("");
	if (morph_buf_init(&out, 256) < 0)
		return strdup("");

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
			morph_buf_append(&out, start, (size_t)(p - start));
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
			morph_buf_append(&out, start, raw_len);
			continue;
		}
		a = turn_artifact_get(j, path, kind);
		if (!a) {
			morph_buf_append(&out, start, raw_len);
			continue;
		}
		append_artifact_tag(&out, a);
		if (strlen(token) < raw_len)
			morph_buf_append(&out, start + strlen(token),
					 raw_len - strlen(token));
	}
	if (out.len == 0) {
		morph_buf_cleanup(&out);
		return strdup(text);
	}
	return morph_buf_detach(&out);
}

static char *turn_render_assistant_for_store(const char *text, void *user_data)
{
	return render_media_refs((struct turn_job *)user_data, text);
}

static char *render_artifact_summary(struct turn_job *j)
{
	morph_buf_t out;

	if (!j || j->artifacts_count <= 0)
		return NULL;
	if (morph_buf_init(&out, 256) < 0)
		return NULL;
	morph_buf_puts(&out,
		   "Generation stopped after reaching the iteration limit, "
		   "but these artifacts were created:\n\n");
	for (int i = 0; i < j->artifacts_count; i++) {
		struct turn_artifact *a = &j->artifacts[i];
		morph_buf_printf(&out, "%d. `%s`\n\n", i + 1, a->path);
		append_artifact_tag(&out, a);
		morph_buf_puts(&out, "\n\n");
	}
	return morph_buf_detach(&out);
}

static int bridge_cb(const struct react_output_event *event, void *u)
{
	struct turn_job *j = (struct turn_job *)u;
	const char *text = event && event->text ? event->text : "";

	if (!event)
		return 0;

	switch (event->type) {
	case REACT_STEP_THOUGHT:
		if (!*text)
			return 0;
		event_sink_thought_turn(j->store, j->session_id, j->turn_id,
					text);
		return 0;

	case REACT_STEP_ACTION:
		if (event->status != REACT_OUTPUT_STARTED ||
		    !event->tool_name || !*event->tool_name)
			return 0;
		snprintf(j->last_tool, sizeof(j->last_tool), "%s",
			 event->tool_name);
		snprintf(j->last_tool_call_id, sizeof(j->last_tool_call_id),
			 "%s", event->tool_call_id ? event->tool_call_id : "");
		event_sink_tool_call_turn(j->store, j->session_id,
					  j->turn_id, event->tool_name,
					  event->tool_args ?
					  event->tool_args : "{}",
					  event->tool_call_id);
		return 0;

	case REACT_STEP_OBSERVATION:
		event_sink_tool_result_turn(j->store, j->session_id,
					    j->turn_id, j->last_tool,
					    text, event->tool_call_id ?
					    event->tool_call_id :
					    j->last_tool_call_id);
		j->last_tool[0] = '\0';
		j->last_tool_call_id[0] = '\0';
		return 0;

	case REACT_STEP_REFLECTION:
		if (!*text)
			return 0;
		event_sink_thought_turn(j->store, j->session_id, j->turn_id,
					text);
		return 0;

	case REACT_STEP_FINAL:
	{
		char *rendered = render_media_refs(j, text);
		event_sink_final_turn(j->store, j->session_id, j->turn_id,
				      rendered ? rendered : text);
		j->final_sent = 1;
		free(rendered);
		return 0;
	}

	case REACT_STEP_REASONING:
		if (!*text)
			return 0;
		event_sink_reasoning_turn(j->store, j->session_id, j->turn_id,
					  text);
		return 0;

	default:
		return 0;
	}
	return 0;
}

static char *event_data_string(cJSON *data, const char *key)
{
	cJSON *item;

	if (!data || !key)
		return NULL;
	item = cJSON_GetObjectItem(data, key);
	if (!item)
		return NULL;
	if (cJSON_IsString(item))
		return strdup(item->valuestring ? item->valuestring : "");
	return cJSON_PrintUnformatted(item);
}

static int bridge_event_cb(const struct morph_event *ev, void *u)
{
	struct turn_job *j = (struct turn_job *)u;
	cJSON *data;
	const char *turn_id;

	if (!j || !ev)
		return -EINVAL;
	data = ev->data;
	turn_id = ev->turn_id ? ev->turn_id : j->turn_id;

	if (strcmp(ev->name, "auth.required") == 0) {
		cJSON *root = cJSON_CreateObject();
		cJSON *copy = data ? cJSON_Duplicate(data, 1) : NULL;
		char *txt;

		if (!root)
			return -ENOMEM;
		cJSON_AddStringToObject(root, "turn_id", turn_id ? turn_id : "");
		if (!copy)
			copy = cJSON_CreateObject();
		if (!copy) {
			cJSON_Delete(root);
			return -ENOMEM;
		}
		cJSON_AddItemToObject(root, "data", copy);
		txt = cJSON_PrintUnformatted(root);
		events_publish(j->store, j->session_id, "auth_required",
			       txt ? txt : "{}");
		free(txt);
		cJSON_Delete(root);
		return 0;
	}

	if (strcmp(ev->name, "react.thought.delta") == 0) {
		char *text = event_data_string(data, "text");
		if (text && *text)
			event_sink_thought_turn(j->store, j->session_id,
						turn_id, text);
		free(text);
		return 0;
	}

	if (strcmp(ev->name, "react.reasoning.delta") == 0) {
		char *text = event_data_string(data, "text");
		if (text && *text)
			event_sink_reasoning_turn(j->store, j->session_id,
						  turn_id, text);
		free(text);
		return 0;
	}

	if (strcmp(ev->name, "react.reflection") == 0) {
		char *text = event_data_string(data, "text");
		if (text && *text)
			event_sink_thought_turn(j->store, j->session_id,
						turn_id, text);
		free(text);
		return 0;
	}

	if (strcmp(ev->name, "tool.call") == 0) {
		char *tool = event_data_string(data, "tool");
		char *args = event_data_string(data, "args");
		char *tool_call_id = event_data_string(data, "tool_call_id");
		event_sink_tool_call_turn(j->store, j->session_id,
					  turn_id, tool ? tool : "",
					  args ? args : "{}",
					  tool_call_id);
		if (tool) {
			snprintf(j->last_tool, sizeof(j->last_tool),
				 "%s", tool);
			snprintf(j->last_tool_call_id,
				 sizeof(j->last_tool_call_id), "%s",
				 tool_call_id ? tool_call_id : "");
		}
		free(tool);
		free(args);
		free(tool_call_id);
		return 0;
	}

	if (strcmp(ev->name, "tool.result") == 0 ||
	    strcmp(ev->name, "tool.failed") == 0) {
		char *tool = event_data_string(data, "tool");
		char *result = event_data_string(data, "result");
		char *tool_call_id = event_data_string(data, "tool_call_id");
		event_sink_tool_result_turn(j->store, j->session_id,
					    turn_id, tool ? tool : j->last_tool,
					    result ? result : "",
					    tool_call_id ? tool_call_id :
					    j->last_tool_call_id);
		j->last_tool[0] = '\0';
		j->last_tool_call_id[0] = '\0';
		free(tool);
		free(result);
		free(tool_call_id);
		return 0;
	}

	if (strcmp(ev->name, "tool.stream.delta") == 0) {
		char *tool = event_data_string(data, "tool");
		char *kind = event_data_string(data, "kind");
		char *text = event_data_string(data, "text");
		char *tool_call_id = event_data_string(data, "tool_call_id");
		event_sink_tool_stream_turn(j->store, j->session_id, turn_id,
					    tool ? tool : j->last_tool,
					    kind ? kind : "text",
					    text ? text : "",
					    tool_call_id ? tool_call_id :
					    j->last_tool_call_id);
		free(tool);
		free(kind);
		free(text);
		free(tool_call_id);
		return 0;
	}

	if (strcmp(ev->name, "react.observation") == 0) {
		cJSON *artifacts = cJSON_GetObjectItem(data, "artifacts");
		cJSON *item;
		if (cJSON_IsArray(artifacts)) {
			cJSON_ArrayForEach(item, artifacts) {
				cJSON *kind = cJSON_GetObjectItem(item, "kind");
				cJSON *path = cJSON_GetObjectItem(item, "path");
				if (cJSON_IsString(path) && path->valuestring) {
					(void)turn_artifact_get(j,
						path->valuestring,
						cJSON_GetStringValue(kind) ?
						cJSON_GetStringValue(kind) :
						"file");
				}
			}
		}
		return 0;
	}

	if (strcmp(ev->name, "artifact.ready") == 0) {
		char *kind = event_data_string(data, "kind");
		char *path = event_data_string(data, "path");

		(void)turn_artifact_get(j, path ? path : "",
					kind ? kind : "file");
		free(kind);
		free(path);
		return 0;
	}

	if (strcmp(ev->name, "react.final") == 0) {
		char *text = event_data_string(data, "text");
		char *rendered = render_media_refs(j, text ? text : "");
		event_sink_final_turn(j->store, j->session_id, turn_id,
				      rendered ? rendered : (text ? text : ""));
		j->final_sent = 1;
		free(rendered);
		free(text);
		return 0;
	}

	if (strcmp(ev->name, "react.failed") == 0 ||
	    strcmp(ev->name, "react.cancelled") == 0 ||
	    strcmp(ev->name, "react.timed_out") == 0 ||
	    strcmp(ev->name, "react.max_iterations") == 0) {
		char *text = event_data_string(data, "text");
		if (text && *text)
			event_sink_error_turn(j->store, j->session_id,
					      turn_id, text);
		free(text);
		return 0;
	}

	return 0;
}

static void *turn_thread(void *arg)
{
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

	current_usage_job = j;
	model_set_usage_callback(record_model_usage);
	model_set_usage_user_data(j);

	if (!react_context_create_for_session) {
		cJSON *err = cJSON_CreateObject();
		char *json = NULL;
		if (err) {
			cJSON_AddStringToObject(err, "turn_id", j->turn_id);
			cJSON_AddStringToObject(err, "message",
				"react integration not linked; see sapi/fastcgi/PATCHES.md §3");
			json = cJSON_PrintUnformatted(err);
			cJSON_Delete(err);
		}
		events_publish(j->store, j->session_id, "error",
			       json ? json : "{\"message\":\"react integration not linked\"}");
		free(json);
		goto out;
	}

	struct react_context *rctx =
		react_context_create_for_session(j->store, j->session_id, j->user_id);
	if (!rctx) {
		cJSON *err = cJSON_CreateObject();
		char *json = NULL;
		if (err) {
			cJSON_AddStringToObject(err, "turn_id", j->turn_id);
			cJSON_AddStringToObject(err, "message",
				"react_context_create_for_session failed");
			json = cJSON_PrintUnformatted(err);
			cJSON_Delete(err);
		}
		events_publish(j->store, j->session_id, "error",
			       json ? json : "{\"message\":\"react_context_create_for_session failed\"}");
		free(json);
		goto out;
	}
	if (react_memory_options_for_session)
		react_memory_options_for_session(&mem_opts);
	if (session_get_by_display_id(&j->store->db, j->session_id, &sess) == 0) {
		struct runtime_engine engine;
		struct runtime_request request;
		struct runtime_result result;
		int run_rc;

		memset(&engine, 0, sizeof(engine));
		runtime_engine_configure(&engine, &j->store->db, rctx, NULL);
		engine.config = fcgi_bridge_config ? fcgi_bridge_config() : NULL;
		memset(&request, 0, sizeof(request));
		request.session_id = sess.id;
		request.model_input = j->input ? j->input : "";
		request.stored_user_input = request.model_input;
		request.turn_id = j->turn_id;
		request.memory_options = &mem_opts;
		request.render_assistant = turn_render_assistant_for_store;
		request.render_user_data = j;
		request.usage_user_data = j;
		request.bind_usage_user_data = 1;
		request.event_cb = react_set_event_callback ? bridge_event_cb : NULL;
		request.event_user_data = j;
		request.output_cb = react_set_event_callback ? NULL : bridge_cb;
		request.output_user_data = j;
		request.turn_flags = AGENT_TURN_DEFAULT_FLAGS;

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

		run_rc = runtime_execute(&engine, &request, &result);
		if (run_rc != 0) {
			cJSON *err = cJSON_CreateObject();
			char *json = NULL;

			if (err) {
				cJSON_AddStringToObject(err, "turn_id",
							j->turn_id);
				cJSON_AddStringToObject(err, "message",
					morph_strerror(run_rc));
				json = cJSON_PrintUnformatted(err);
				cJSON_Delete(err);
			}
			events_publish(j->store, j->session_id, "error",
				       json ? json :
				       "{\"message\":\"turn failed\"}");
			free(json);
			goto out_destroy;
		}
		if (!j->final_sent) {
			char *fallback = render_artifact_summary(j);
			if (!fallback && rctx->final_answer)
				fallback = render_media_refs(j, rctx->final_answer);
			if (fallback && *fallback) {
				event_sink_final_turn(j->store, j->session_id,
						      j->turn_id, fallback);
				j->final_sent = 1;
			}
			free(fallback);
		}
	} else {
		cJSON *err = cJSON_CreateObject();
		char *json = NULL;

		if (err) {
			cJSON_AddStringToObject(err, "turn_id", j->turn_id);
			cJSON_AddStringToObject(err, "message",
						"session not found");
			json = cJSON_PrintUnformatted(err);
			cJSON_Delete(err);
		}
		events_publish(j->store, j->session_id, "error",
			       json ? json : "{\"message\":\"session not found\"}");
		free(json);
		goto out_destroy;
	}
	{
		cJSON *end = cJSON_CreateObject();
		char *json = NULL;
		if (end) {
			cJSON_AddStringToObject(end, "phase", "done");
			cJSON_AddStringToObject(end, "turn_id", j->turn_id);
			json = cJSON_PrintUnformatted(end);
			cJSON_Delete(end);
		}
		events_publish(j->store, j->session_id, "turn_end",
			       json ? json : "{\"phase\":\"done\"}");
		free(json);
	}

out_destroy:
	if (react_context_destroy) react_context_destroy(rctx);
out:
	model_set_usage_user_data(NULL);
	current_usage_job = NULL;
	store_quota_end_turn(j->store, j->turn_id);
	free(j->input);
	free(j);
	return NULL;
}

void handle_post_turn(request_t *r)
{
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
