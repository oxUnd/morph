/* event_sink.c — produce JSON event payloads. */
#include "event_sink.h"
#include "sapi/fastcgi/storage/events.h"

#include <stdlib.h>

#include "util/json.h"

static void publish_obj(struct session_store *s, const char *sid, const char *type,
			struct json *root)
{
	char *txt = json_stringify(root);
	events_publish(s, sid, type, txt ? txt : "{}");
	free(txt);
	json_free(root);
}

static void add_turn_id(struct json *o, const char *turn_id)
{
	if (turn_id && *turn_id)
		json_add_string(o, "turn_id", turn_id);
}

static void add_tool_call_id(struct json *o, const char *tool_call_id)
{
	if (tool_call_id && *tool_call_id)
		json_add_string(o, "tool_call_id", tool_call_id);
}

void event_sink_thought(struct session_store *s, const char *sid, const char *text)
{
	event_sink_thought_turn(s, sid, NULL, text);
}

void event_sink_thought_turn(struct session_store *s, const char *sid, const char *turn_id,
			     const char *text)
{
	struct json *o = json_object();
	add_turn_id(o, turn_id);
	json_add_string(o, "text", text ? text : "");
	publish_obj(s, sid, "thought", o);
}

void event_sink_reasoning(struct session_store *s, const char *sid, const char *text)
{
	event_sink_reasoning_turn(s, sid, NULL, text);
}

void event_sink_reasoning_turn(struct session_store *s, const char *sid, const char *turn_id,
			       const char *text)
{
	struct json *o = json_object();
	add_turn_id(o, turn_id);
	json_add_string(o, "text", text ? text : "");
	publish_obj(s, sid, "reasoning", o);
}

void event_sink_tool_call(struct session_store *s, const char *sid, const char *tool,
			  const char *args_json)
{
	event_sink_tool_call_turn(s, sid, NULL, tool, args_json, NULL);
}

void event_sink_tool_call_turn(struct session_store *s, const char *sid, const char *turn_id,
			       const char *tool, const char *args_json, const char *tool_call_id)
{
	struct json *o = json_object();
	add_turn_id(o, turn_id);
	add_tool_call_id(o, tool_call_id);
	json_add_string(o, "tool", tool ? tool : "");
	struct json *args = args_json ? json_parse_prefix(args_json, NULL, NULL) : NULL;
	if (!args)
		args = json_object();
	if (json_object_add(o, "args", args))
		json_free(args);
	publish_obj(s, sid, "tool_call", o);
}

void event_sink_tool_result(struct session_store *s, const char *sid, const char *tool,
			    const char *result_json)
{
	event_sink_tool_result_turn(s, sid, NULL, tool, result_json, NULL);
}

void event_sink_tool_result_turn(struct session_store *s, const char *sid, const char *turn_id,
				 const char *tool, const char *result_json,
				 const char *tool_call_id)
{
	struct json *o = json_object();
	add_turn_id(o, turn_id);
	add_tool_call_id(o, tool_call_id);
	json_add_string(o, "tool", tool ? tool : "");
	struct json *res = result_json ? json_parse_prefix(result_json, NULL, NULL) : NULL;
	if (!res) {
		json_add_string(o, "result", result_json ? result_json : "");
	} else {
		if (json_object_add(o, "result", res))
			json_free(res);
	}
	publish_obj(s, sid, "tool_result", o);
}

void event_sink_tool_stream_turn(struct session_store *s, const char *sid, const char *turn_id,
				 const char *tool, const char *kind, const char *text,
				 const char *tool_call_id)
{
	struct json *o = json_object();
	add_turn_id(o, turn_id);
	add_tool_call_id(o, tool_call_id);
	json_add_string(o, "tool", tool ? tool : "");
	json_add_string(o, "kind", kind ? kind : "text");
	json_add_string(o, "text", text ? text : "");
	publish_obj(s, sid, "tool_stream", o);
}

void event_sink_final(struct session_store *s, const char *sid, const char *text)
{
	event_sink_final_turn(s, sid, NULL, text);
}

void event_sink_final_turn(struct session_store *s, const char *sid, const char *turn_id,
			   const char *text)
{
	struct json *o = json_object();
	add_turn_id(o, turn_id);
	json_add_string(o, "text", text ? text : "");
	publish_obj(s, sid, "final", o);
}

void event_sink_error(struct session_store *s, const char *sid, const char *msg)
{
	event_sink_error_turn(s, sid, NULL, msg);
}

void event_sink_error_turn(struct session_store *s, const char *sid, const char *turn_id,
			   const char *msg)
{
	struct json *o = json_object();
	add_turn_id(o, turn_id);
	json_add_string(o, "message", msg ? msg : "unknown error");
	publish_obj(s, sid, "error", o);
}
