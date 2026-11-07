#include "sapi/fastcgi/adapters/turn_events.h"
#include "sapi/fastcgi/services/turn_job_internal.h"
#include "sapi/fastcgi/storage/events.h"
#include "sapi/fastcgi/event_sink.h"
#include "cJSON.h"
#include "util/error.h"
#include "util/child_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sapi/fastcgi/presentation/media_refs.h"
#include "event/event.h"

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

static int64_t turn_mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void turn_flush_thought(struct fcgi_turn_job *j)
{
	if (!j || j->pending_thought_len == 0)
		return;
	j->pending_thought[j->pending_thought_len] = '\0';
	event_sink_thought_turn(j->store, j->session_id, j->turn_id, j->pending_thought);
	j->pending_thought_len = 0;
}

static void turn_flush_reasoning(struct fcgi_turn_job *j)
{
	if (!j || j->pending_reasoning_len == 0)
		return;
	j->pending_reasoning[j->pending_reasoning_len] = '\0';
	event_sink_reasoning_turn(j->store, j->session_id, j->turn_id, j->pending_reasoning);
	j->pending_reasoning_len = 0;
}

static void turn_flush_tool_stream(struct fcgi_turn_job *j)
{
	if (!j || j->pending_tool_stream_len == 0)
		return;
	j->pending_tool_stream[j->pending_tool_stream_len] = '\0';
	event_sink_tool_stream_turn(j->store, j->session_id, j->turn_id, j->pending_tool_name,
				    j->pending_tool_kind, j->pending_tool_stream,
				    j->pending_tool_call_id);
	j->pending_tool_stream_len = 0;
}

static void turn_flush_pending(struct fcgi_turn_job *j)
{
	turn_flush_thought(j);
	turn_flush_reasoning(j);
	turn_flush_tool_stream(j);
}

static void turn_pending_append(char *buf, size_t *len, int64_t *started, const char *text)
{
	size_t n;

	if (!text || !*text)
		return;
	n = strlen(text);
	if (*len == 0)
		*started = turn_mono_ms();
	memcpy(buf + *len, text, n);
	*len += n;
}

int fcgi_turn_events_callback(const struct morph_event *ev, void *u)
{
	struct fcgi_turn_job *j = (struct fcgi_turn_job *)u;
	cJSON *data;
	const char *turn_id;

	if (!j || !ev)
		return -EINVAL;
	data = ev->data;
	turn_id = ev->turn_id ? ev->turn_id : j->turn_id;

	if (strcmp(ev->name, "react.thought.delta") != 0 &&
	    strcmp(ev->name, "react.content.delta") != 0 &&
	    strcmp(ev->name, "react.reasoning.delta") != 0 &&
	    strcmp(ev->name, "tool.stream.delta") != 0)
		turn_flush_pending(j);

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
		events_publish(j->store, j->session_id, "auth_required", txt ? txt : "{}");
		free(txt);
		cJSON_Delete(root);
		return 0;
	}

	if (strcmp(ev->name, "react.thought.delta") == 0 ||
	    strcmp(ev->name, "react.content.delta") == 0) {
		char *text = event_data_string(data, "text");

		if (text && *text) {
			size_t n = strlen(text);

			if (j->pending_thought_len + n + 1 > sizeof(j->pending_thought))
				turn_flush_thought(j);
			if (n + 1 > sizeof(j->pending_thought)) {
				event_sink_thought_turn(j->store, j->session_id, turn_id, text);
			} else {
				turn_pending_append(j->pending_thought, &j->pending_thought_len,
						    &j->pending_thought_ms, text);
				if (j->pending_thought_len >= sizeof(j->pending_thought) - 64 ||
				    turn_mono_ms() - j->pending_thought_ms >= 100)
					turn_flush_thought(j);
			}
		}
		free(text);
		return 0;
	}

	if (strcmp(ev->name, "react.reasoning.delta") == 0) {
		char *text = event_data_string(data, "text");

		if (text && *text) {
			size_t n = strlen(text);

			if (j->pending_reasoning_len + n + 1 > sizeof(j->pending_reasoning))
				turn_flush_reasoning(j);
			if (n + 1 > sizeof(j->pending_reasoning)) {
				event_sink_reasoning_turn(j->store, j->session_id, turn_id, text);
			} else {
				turn_pending_append(j->pending_reasoning, &j->pending_reasoning_len,
						    &j->pending_reasoning_ms, text);
				if (j->pending_reasoning_len >= sizeof(j->pending_reasoning) - 64 ||
				    turn_mono_ms() - j->pending_reasoning_ms >= 100)
					turn_flush_reasoning(j);
			}
		}
		free(text);
		return 0;
	}

	if (strcmp(ev->name, "react.reflection") == 0) {
		char *text = event_data_string(data, "text");
		if (text && *text)
			event_sink_thought_turn(j->store, j->session_id, turn_id, text);
		free(text);
		return 0;
	}

	if (strcmp(ev->name, "tool.call") == 0) {
		char *tool = event_data_string(data, "tool");
		char *args = event_data_string(data, "args");
		char *tool_call_id = event_data_string(data, "tool_call_id");
		event_sink_tool_call_turn(j->store, j->session_id, turn_id, tool ? tool : "",
					  args ? args : "{}", tool_call_id);
		if (tool) {
			snprintf(j->last_tool, sizeof(j->last_tool), "%s", tool);
			snprintf(j->last_tool_call_id, sizeof(j->last_tool_call_id), "%s",
				 tool_call_id ? tool_call_id : "");
		}
		free(tool);
		free(args);
		free(tool_call_id);
		return 0;
	}

	if (strcmp(ev->name, "tool.result") == 0 || strcmp(ev->name, "tool.failed") == 0) {
		char *tool = event_data_string(data, "tool");
		char *result = event_data_string(data, "result");
		char *tool_call_id = event_data_string(data, "tool_call_id");
		event_sink_tool_result_turn(j->store, j->session_id, turn_id,
					    tool ? tool : j->last_tool, result ? result : "",
					    tool_call_id ? tool_call_id : j->last_tool_call_id);
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
					    tool ? tool : j->last_tool, kind ? kind : "text",
					    text ? text : "",
					    tool_call_id ? tool_call_id : j->last_tool_call_id);
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
			cJSON_ArrayForEach (item, artifacts) {
				cJSON *kind = cJSON_GetObjectItem(item, "kind");
				cJSON *path = cJSON_GetObjectItem(item, "path");
				if (cJSON_IsString(path) && path->valuestring) {
					(void)fcgi_media_artifact_get(
						j, path->valuestring,
						cJSON_GetStringValue(kind)
							? cJSON_GetStringValue(kind)
							: "file");
				}
			}
		}
		return 0;
	}

	if (strcmp(ev->name, "artifact.ready") == 0) {
		char *kind = event_data_string(data, "kind");
		char *path = event_data_string(data, "path");

		(void)fcgi_media_artifact_get(j, path ? path : "", kind ? kind : "file");
		free(kind);
		free(path);
		return 0;
	}

	if (strcmp(ev->name, "react.final") == 0) {
		char *text = event_data_string(data, "text");
		char *rendered = fcgi_media_refs_render(j, text ? text : "");
		event_sink_final_turn(j->store, j->session_id, turn_id,
				      rendered ? rendered : (text ? text : ""));
		j->final_sent = 1;
		free(rendered);
		free(text);
		return 0;
	}

	if (strcmp(ev->name, "react.failed") == 0 || strcmp(ev->name, "react.cancelled") == 0 ||
	    strcmp(ev->name, "react.timed_out") == 0 ||
	    strcmp(ev->name, "react.max_iterations") == 0) {
		char *text = event_data_string(data, "text");
		if (text && *text)
			event_sink_error_turn(j->store, j->session_id, turn_id, text);
		free(text);
		return 0;
	}

	return 0;
}
