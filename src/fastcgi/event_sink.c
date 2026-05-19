/* event_sink.c — produce JSON-shaped event payloads via cJSON */
#include "event_sink.h"

#include <stdlib.h>

#include "cJSON.h"

static void publish_obj(struct session_store *s, const char *sid,
			const char *type, cJSON *root) {
	char *txt = cJSON_PrintUnformatted(root);
	events_publish(s, sid, type, txt ? txt : "{}");
	free(txt);
	cJSON_Delete(root);
}

void event_sink_thought(struct session_store *s, const char *sid, const char *text) {
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "text", text ? text : "");
	publish_obj(s, sid, "thought", o);
}

void event_sink_tool_call(struct session_store *s, const char *sid,
			  const char *tool, const char *args_json) {
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "tool", tool ? tool : "");
	cJSON *args = args_json ? cJSON_Parse(args_json) : NULL;
	if (!args) args = cJSON_CreateObject();
	cJSON_AddItemToObject(o, "args", args);
	publish_obj(s, sid, "tool_call", o);
}

void event_sink_tool_result(struct session_store *s, const char *sid,
			    const char *tool, const char *result_json) {
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "tool", tool ? tool : "");
	cJSON *res = result_json ? cJSON_Parse(result_json) : NULL;
	if (!res) {
		cJSON_AddStringToObject(o, "result", result_json ? result_json : "");
	} else {
		cJSON_AddItemToObject(o, "result", res);
	}
	publish_obj(s, sid, "tool_result", o);
}

void event_sink_final(struct session_store *s, const char *sid, const char *text) {
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "text", text ? text : "");
	publish_obj(s, sid, "final", o);
}

void event_sink_error(struct session_store *s, const char *sid, const char *msg) {
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "message", msg ? msg : "unknown error");
	publish_obj(s, sid, "error", o);
}
