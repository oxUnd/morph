/* event_sink.h — convenience helpers around events_publish */
#ifndef MORPH_FCGI_EVENT_SINK_H
#define MORPH_FCGI_EVENT_SINK_H

#include "session_store.h"

void event_sink_thought    (struct session_store *s, const char *sid, const char *text);
void event_sink_reasoning  (struct session_store *s, const char *sid, const char *text);
void event_sink_tool_call  (struct session_store *s, const char *sid,
			    const char *tool, const char *args_json);
void event_sink_tool_result(struct session_store *s, const char *sid,
			    const char *tool, const char *result_json);
void event_sink_final      (struct session_store *s, const char *sid, const char *text);
void event_sink_error      (struct session_store *s, const char *sid, const char *msg);

void event_sink_thought_turn(struct session_store *s, const char *sid,
			     const char *turn_id, const char *text);
void event_sink_reasoning_turn(struct session_store *s, const char *sid,
			       const char *turn_id, const char *text);
void event_sink_tool_call_turn(struct session_store *s, const char *sid,
			       const char *turn_id, const char *tool,
			       const char *args_json,
			       const char *tool_call_id);
void event_sink_tool_result_turn(struct session_store *s, const char *sid,
				 const char *turn_id, const char *tool,
				 const char *result_json,
				 const char *tool_call_id);
void event_sink_tool_stream_turn(struct session_store *s, const char *sid,
				 const char *turn_id, const char *tool,
				 const char *kind, const char *text,
				 const char *tool_call_id);
void event_sink_final_turn(struct session_store *s, const char *sid,
			   const char *turn_id, const char *text);
void event_sink_error_turn(struct session_store *s, const char *sid,
			   const char *turn_id, const char *msg);

#endif
