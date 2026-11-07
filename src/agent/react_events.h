#ifndef MORPH_REACT_EVENTS_H
#define MORPH_REACT_EVENTS_H

#include "event/event.h"
#include "agent/tool_artifact.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tool_registry;

/* Borrowed, synchronous destination; no execution state or lifecycle ownership. */
struct react_event_sink {
	morph_event_cb callback;
	void *user_data;
	const char *turn_id;
	struct tool_registry *tools;
};

int react_events_enabled(struct react_event_sink sink);
int react_emit_event(struct react_event_sink sink, enum morph_event_type type, const char *name,
		     const char *phase, const char *message, struct json *data);
int react_emit_text_event(struct react_event_sink sink, enum morph_event_type type,
			  const char *name, const char *phase, const char *message,
			  const char *text);
int react_emit_observation_event(struct react_event_sink sink, const char *text, const char *tool,
				 int error_code, const struct tool_artifact_list *artifacts,
				 const struct json *result_data, const struct json *result_ui);
int react_emit_auth_required(struct react_event_sink sink, const char *backend,
			     const char *provider, const char *model, const char *tool,
			     const char *reason);
int react_emit_tool_event(struct react_event_sink sink, const char *name, const char *phase,
			  const char *message, const char *tool, const char *args_json,
			  const char *tool_call_id, const char *result, int error_code);
int react_emit_artifact_event(struct react_event_sink sink, const char *kind, const char *path,
			      const char *source);
void react_emit_artifacts_from_list(struct react_event_sink sink,
				    const struct tool_artifact_list *artifacts, const char *source);
int react_emit_hitl_event(struct react_event_sink sink, const char *name, const char *phase,
			  const char *message, const char *tool, const char *args_json,
			  const char *tool_call_id, const char *verdict);

#ifdef __cplusplus
}
#endif

#endif
