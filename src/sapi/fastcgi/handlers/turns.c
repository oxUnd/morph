/* turns.c — POST /api/sessions/:id/turns
 *
 * Spawns a worker thread that drives one ReAct round through the shared
 * runtime layer and bridges ReAct events into
 * the FastCGI event store.  Web clients see the stream via the SSE endpoint.
 *
 * Execution is delegated to the shared runtime used by every front end.
 */
#define _GNU_SOURCE
#include "handlers.h"
#include "../action_pump.h"
#include "../agent_bridge.h"
#include "../session_store.h"
#include "../event_sink.h"
#include "agent/react.h"
#include "agent/tool_context.h"
#include "agent/turn.h"
#include "event/event.h"
#include "runtime/runtime.h"
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

#define FCGI_ACTION_WAIT_SECONDS 300

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
	char action_type[32];
	char *action_payload;
	char pending_type[32];
	char *pending_payload;
};

static const char *bridge_output_dir(void)
{
	return fcgi_artifact_output_dir();
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

static int bridge_memory_session_visible(const char *display_id,
					 const char *user_id,
					 void *user_data)
{
	struct session_store *store = user_data;

	if (!store || !display_id || !user_id)
		return 0;
	return store_session_owned_by(store, display_id, user_id);
}

static void turn_action_record_cleanup(struct action_record *record)
{
	if (!record)
		return;
	free(record->payload_json);
	memset(record, 0, sizeof(*record));
}

static int turn_take_pending_action(struct turn_job *j,
				    struct action_record *record)
{
	if (!j || !record || !j->pending_type[0])
		return 0;
	memset(record, 0, sizeof(*record));
	snprintf(record->type, sizeof(record->type), "%s", j->pending_type);
	record->payload_json = j->pending_payload;
	j->pending_type[0] = '\0';
	j->pending_payload = NULL;
	return 1;
}

static void turn_save_pending_action(struct turn_job *j,
				     struct action_record *record)
{
	if (!j || !record)
		return;
	if (j->pending_type[0]) {
		turn_action_record_cleanup(record);
		return;
	}
	snprintf(j->pending_type, sizeof(j->pending_type), "%s",
		 record->type);
	j->pending_payload = record->payload_json;
	record->payload_json = NULL;
}

static int fcgi_turn_action_drain(void *user_data, struct react_action *out,
				  int timeout_sec)
{
	struct turn_job *j = user_data;
	struct action_record record;
	int got;

	if (!j || !out)
		return -EINVAL;
	free(j->action_payload);
	j->action_payload = NULL;
	j->action_type[0] = '\0';
	memset(&record, 0, sizeof(record));
	got = action_pump_wait(j->store, j->session_id, timeout_sec, &record);
	if (got <= 0)
		return got;
	if (strcmp(record.type, "approve") == 0 ||
	    strcmp(record.type, "reject") == 0 ||
	    strcmp(record.type, "always") == 0) {
		turn_save_pending_action(j, &record);
		return 0;
	}
	snprintf(j->action_type, sizeof(j->action_type), "%s", record.type);
	j->action_payload = record.payload_json;
	out->type = j->action_type;
	out->payload_json = j->action_payload;
	return 1;
}

static void publish_interaction(struct turn_job *j, const char *type,
				const char *message, cJSON *data)
{
	cJSON *root;
	char *json;

	if (!j || !type)
		return;
	root = cJSON_CreateObject();
	if (!root)
		return;
	cJSON_AddStringToObject(root, "turn_id", j->turn_id);
	if (message)
		cJSON_AddStringToObject(root, "message", message);
	if (data)
		cJSON_AddItemToObject(root, "data", data);
	json = cJSON_PrintUnformatted(root);
	events_publish(j->store, j->session_id, type, json ? json : "{}");
	free(json);
	cJSON_Delete(root);
}

static int action_answers(const char *payload, char ***answers,
			  int *answers_count)
{
	cJSON *root;
	cJSON *items;
	cJSON *text;
	char **values;
	int count;
	int written = 0;

	if (!answers || !answers_count)
		MORPH_RETURN(-EINVAL);
	*answers = NULL;
	*answers_count = 0;
	root = cJSON_Parse(payload ? payload : "{}");
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		MORPH_RETURN(-EINVAL);
	}
	items = cJSON_GetObjectItem(root, "answers");
	text = cJSON_GetObjectItem(root, "text");
	count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) :
		(cJSON_IsString(text) ? 1 : 0);
	if (count > 64)
		count = 64;
	if (count == 0) {
		cJSON_Delete(root);
		return 0;
	}
	values = calloc((size_t)count, sizeof(*values));
	if (!values) {
		cJSON_Delete(root);
		MORPH_RETURN(-ENOMEM);
	}
	for (int i = 0; i < count; i++) {
		cJSON *item = cJSON_IsArray(items)
			? cJSON_GetArrayItem(items, i) : text;
		if (!cJSON_IsString(item) || !item->valuestring ||
		    !item->valuestring[0])
			continue;
		values[written] = strdup(item->valuestring);
		if (!values[written]) {
			for (int k = 0; k < written; k++)
				free(values[k]);
			free(values);
			cJSON_Delete(root);
			MORPH_RETURN(-ENOMEM);
		}
		written++;
	}
	cJSON_Delete(root);
	*answers = values;
	*answers_count = written;
	return 0;
}

int fcgi_turn_ask_user(const char *question,
		       const char *const *choices, int choices_count,
		       const char *selection_mode, int min_choices,
		       int max_choices, char ***answers, int *answers_count,
		       void *turn_data)
{
	struct turn_job *j = turn_data;
	struct action_record record;
	cJSON *data;
	cJSON *array;
	int got;

	(void)selection_mode;
	(void)min_choices;
	(void)max_choices;
	if (!j || !answers || !answers_count)
		MORPH_RETURN(-EINVAL);
	data = cJSON_CreateObject();
	array = cJSON_CreateArray();
	if (data && array) {
		cJSON_AddStringToObject(data, "question", question ? question : "");
		for (int i = 0; i < choices_count; i++)
			cJSON_AddItemToArray(array,
				cJSON_CreateString(choices[i] ? choices[i] : ""));
		cJSON_AddItemToObject(data, "choices", array);
		publish_interaction(j, "ask_user", "user input required", data);
	} else {
		cJSON_Delete(array);
		cJSON_Delete(data);
	}
	memset(&record, 0, sizeof(record));
	got = action_pump_wait(j->store, j->session_id,
			       FCGI_ACTION_WAIT_SECONDS, &record);
	if (got <= 0)
		MORPH_RETURN(got < 0 ? got : -ETIMEDOUT);
	if (strcmp(record.type, "cancel") == 0) {
		turn_action_record_cleanup(&record);
		MORPH_RETURN(-ECANCELED);
	}
	if (strcmp(record.type, "prompt") != 0 &&
	    strcmp(record.type, "answer") != 0) {
		turn_save_pending_action(j, &record);
		MORPH_RETURN(-EAGAIN);
	}
	got = action_answers(record.payload_json, answers, answers_count);
	turn_action_record_cleanup(&record);
	return got;
}

static int action_is_always(const char *payload)
{
	cJSON *root = cJSON_Parse(payload ? payload : "{}");
	cJSON *always = cJSON_IsObject(root)
		? cJSON_GetObjectItem(root, "always") : NULL;
	int result = cJSON_IsTrue(always);

	cJSON_Delete(root);
	return result;
}

static enum hitl_verdict fcgi_turn_hitl(const char *tool_name,
					const char *tool_args,
					void *turn_data)
{
	struct turn_job *j = turn_data;
	struct action_record record;
	cJSON *data;
	int got;

	if (!j)
		return HITL_DENY;
	data = cJSON_CreateObject();
	if (data) {
		cJSON_AddStringToObject(data, "tool", tool_name ? tool_name : "");
		cJSON_AddStringToObject(data, "args", tool_args ? tool_args : "{}");
		publish_interaction(j, "approval_required",
				    "tool approval required", data);
	}
	memset(&record, 0, sizeof(record));
	got = turn_take_pending_action(j, &record);
	if (!got)
		got = action_pump_wait(j->store, j->session_id,
			       FCGI_ACTION_WAIT_SECONDS, &record);
	if (got <= 0)
		return HITL_DENY;
	if (strcmp(record.type, "always") == 0 ||
	    (strcmp(record.type, "approve") == 0 &&
	     action_is_always(record.payload_json))) {
		turn_action_record_cleanup(&record);
		return HITL_ALWAYS;
	}
	got = strcmp(record.type, "approve") == 0;
	turn_action_record_cleanup(&record);
	return got ? HITL_APPROVE : HITL_DENY;
}

enum tool_operation_verdict
fcgi_turn_operation_approval(const struct tool_operation *op, void *turn_data)
{
	struct turn_job *j = turn_data;
	struct action_record record;
	cJSON *data;
	int got;

	if (!j || !op)
		return TOOL_OP_DENY;
	data = cJSON_CreateObject();
	if (data) {
		cJSON_AddStringToObject(data, "tool",
			op->tool_name ? op->tool_name : "");
		cJSON_AddStringToObject(data, "action",
			op->action ? op->action : "");
		cJSON_AddStringToObject(data, "target",
			op->target ? op->target : "");
		publish_interaction(j, "operation_approval_required",
				    "operation approval required", data);
	}
	memset(&record, 0, sizeof(record));
	got = turn_take_pending_action(j, &record);
	if (!got)
		got = action_pump_wait(j->store, j->session_id,
			       FCGI_ACTION_WAIT_SECONDS, &record);
	if (got <= 0)
		return TOOL_OP_DENY;
	if (strcmp(record.type, "always") == 0 ||
	    (strcmp(record.type, "approve") == 0 &&
	     action_is_always(record.payload_json))) {
		turn_action_record_cleanup(&record);
		return TOOL_OP_ALWAYS;
	}
	got = strcmp(record.type, "approve") == 0;
	turn_action_record_cleanup(&record);
	return got ? TOOL_OP_ALLOW : TOOL_OP_DENY;
}

static void *turn_thread(void *arg)
{
	struct turn_job *j = (struct turn_job *)arg;
	struct session sess = {0};
	struct runtime *runtime = NULL;
	struct runtime_request request;
	struct runtime_result result;
	int run_rc;

	memset(&result, 0, sizeof(result));
	if (session_get_by_display_id(&j->store->db, j->session_id, &sess) == 0) {
		memset(&request, 0, sizeof(request));
		request.session_id = sess.id;
		request.model_input = j->input ? j->input : "";
		request.stored_user_input = request.model_input;
		request.turn_id = j->turn_id;
		request.render_assistant = turn_render_assistant_for_store;
		request.render_user_data = j;
		request.user_id = j->user_id;
		request.restrict_memory_to_user = 1;
		request.memory_visible_fn = bridge_memory_session_visible;
		request.memory_visible_user_data = j->store;
		request.event_cb = bridge_event_cb;
		request.event_user_data = j;
		request.hitl_cb = fcgi_turn_hitl;
		request.hitl_user_data = j;
		request.override_hitl = 1;
		request.action_drain_fn = fcgi_turn_action_drain;
		request.action_drain_user_data = j;
		request.override_action_drain = 1;
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

		run_rc = fcgi_bridge_runtime_acquire(j->session_id, &runtime);
		if (run_rc == 0)
			run_rc = runtime_execute_turn(runtime, &request, &result);
		fcgi_bridge_runtime_release(runtime);
		runtime = NULL;
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
			goto out;
		}
		if (!j->final_sent) {
			char *fallback = render_artifact_summary(j);
			if (!fallback && result.final_text)
				fallback = render_media_refs(j, result.final_text);
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
		goto out;
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

out:
	fcgi_bridge_runtime_release(runtime);
	store_quota_end_turn(j->store, j->turn_id);
	fcgi_bridge_turn_end();
	free(j->action_payload);
	free(j->pending_payload);
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
	if (fcgi_bridge_turn_begin() != 0) {
		store_quota_end_turn(r->store, j->turn_id);
		free(j->input); free(j); cJSON_Delete(root); free(body);
		reply_json(r, 503, "{\"error\":\"shutting_down\"}");
		return;
	}

	pthread_t tid;
	if (pthread_create(&tid, NULL, turn_thread, j) != 0) {
		fcgi_bridge_turn_end();
		store_quota_end_turn(r->store, j->turn_id);
		free(j->input); free(j); cJSON_Delete(root); free(body);
		reply_500(r, "thread spawn"); return;
	}
	pthread_detach(tid);

	cJSON_Delete(root);
	free(body);
	reply_202_json(r, "{\"accepted\":true}");
}
