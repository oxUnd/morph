/* event_sink.h — convenience helpers around events_publish */
#ifndef MORPH_FCGI_EVENT_SINK_H
#define MORPH_FCGI_EVENT_SINK_H

#include "session_store.h"

void event_sink_thought    (struct session_store *s, const char *sid, const char *text);
void event_sink_tool_call  (struct session_store *s, const char *sid, const char *tool, const char *args_json);
void event_sink_tool_result(struct session_store *s, const char *sid, const char *tool, const char *result_json);
void event_sink_final      (struct session_store *s, const char *sid, const char *text);
void event_sink_error      (struct session_store *s, const char *sid, const char *msg);

#endif
