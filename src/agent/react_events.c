#include "react_events.h"

#include "agent/tool_registry.h"
#include "util/error.h"
#include <string.h>
#include <errno.h>

int react_events_enabled(struct react_event_sink sink)
{
	return sink.callback != NULL;
}

int react_emit_event(struct react_event_sink sink, enum morph_event_type type, const char *name,
		     const char *phase, const char *message, cJSON *data)
{
	struct morph_event ev;

	if (!sink.callback)
		return 0;
	ev.type = type;
	ev.name = name;
	ev.phase = phase;
	ev.message = message;
	ev.data = data;
	ev.turn_id = sink.turn_id && *sink.turn_id ? sink.turn_id : NULL;
	return morph_event_emit(sink.callback, sink.user_data, &ev);
}

int react_emit_text_event(struct react_event_sink sink, enum morph_event_type type,
			  const char *name, const char *phase, const char *message,
			  const char *text)
{
	cJSON *data;
	int rc;

	if (!react_events_enabled(sink))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "text", text ? text : "");
	rc = react_emit_event(sink, type, name, phase, message, data);
	cJSON_Delete(data);
	return rc;
}

int react_emit_observation_event(struct react_event_sink sink, const char *text, const char *tool,
				 int error_code, const struct tool_artifact_list *artifacts,
				 const cJSON *result_data, const cJSON *result_ui)
{
	cJSON *data;
	cJSON *artifact_json = NULL;
	cJSON *data_copy = NULL;
	cJSON *ui_copy = NULL;
	int rc;

	if (!react_events_enabled(sink))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "text", text ? text : "");
	if (tool)
		cJSON_AddStringToObject(data, "tool", tool);
	if (error_code < 0) {
		cJSON_AddNumberToObject(data, "error_code", error_code);
		cJSON_AddStringToObject(data, "error", morph_strerror(error_code));
	}
	if (artifacts && artifacts->count > 0) {
		artifact_json = tool_artifact_list_json_create(artifacts);
		if (artifact_json)
			cJSON_AddItemToObject(data, "artifacts", artifact_json);
	}
	if (result_data) {
		data_copy = cJSON_Duplicate(result_data, 1);
		if (data_copy)
			cJSON_AddItemToObject(data, "data", data_copy);
	}
	if (result_ui) {
		ui_copy = cJSON_Duplicate(result_ui, 1);
		if (ui_copy)
			cJSON_AddItemToObject(data, "ui", ui_copy);
	}
	rc = react_emit_event(sink, MORPH_EVENT_REACT, "react.observation",
			      error_code < 0 ? "failed" : "end", "tool observation", data);
	cJSON_Delete(data);
	return rc;
}

int react_emit_auth_required(struct react_event_sink sink, const char *backend,
			     const char *provider, const char *model, const char *tool,
			     const char *reason)
{
	cJSON *data;
	int rc;

	if (!react_events_enabled(sink))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "kind", "auth_required");
	cJSON_AddStringToObject(data, "backend", backend ? backend : "");
	cJSON_AddStringToObject(data, "provider", provider ? provider : "");
	cJSON_AddStringToObject(data, "model", model ? model : "");
	cJSON_AddStringToObject(data, "env_name", "");
	cJSON_AddStringToObject(data, "tool", tool ? tool : "");
	cJSON_AddStringToObject(data, "reason", reason ? reason : "missing_api_key");
	cJSON_AddBoolToObject(data, "retryable", 1);
	rc = react_emit_event(sink, MORPH_EVENT_HITL, "auth.required", "blocked",
			      "authentication required", data);
	cJSON_Delete(data);
	return rc;
}

int react_emit_tool_event(struct react_event_sink sink, const char *name, const char *phase,
			  const char *message, const char *tool, const char *args_json,
			  const char *tool_call_id, const char *result, int error_code)
{
	cJSON *data;
	cJSON *args = NULL;
	int text_input = 0;
	int rc;

	if (!react_events_enabled(sink))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "tool", tool ? tool : "");
	if (sink.tools && tool) {
		tool_registry_lock(sink.tools);
		struct tool_entry *entry = tool_borrow(sink.tools, tool);

		if (entry && entry->desc.title[0])
			cJSON_AddStringToObject(data, "toolTitle", entry->desc.title);
		if (entry && entry->desc.input_kind == TOOL_INPUT_TEXT)
			text_input = 1;
		tool_registry_unlock(sink.tools);
	}
	cJSON_AddStringToObject(data, "tool_call_id", tool_call_id ? tool_call_id : "");
	if (text_input) {
		args = cJSON_CreateObject();
		if (args)
			cJSON_AddStringToObject(args, "input", args_json ? args_json : "");
	} else {
		args = args_json && *args_json ? cJSON_Parse(args_json) : NULL;
	}
	if (!args)
		args = cJSON_CreateObject();
	if (!args) {
		cJSON_Delete(data);
		return -ENOMEM;
	}
	cJSON_AddItemToObject(data, "args", args);
	if (result)
		cJSON_AddStringToObject(data, "result", result);
	if (error_code < 0) {
		const char *error = morph_strerror(error_code);

		if (result && result[0]) {
			static const char prefix[] = "tool error: ";

			error = strncmp(result, prefix, sizeof(prefix) - 1) == 0
				    ? result + sizeof(prefix) - 1
				    : result;
		}
		cJSON_AddNumberToObject(data, "error_code", error_code);
		cJSON_AddStringToObject(data, "error", error);
	}
	rc = react_emit_event(sink, MORPH_EVENT_TOOL, name, phase, message, data);
	cJSON_Delete(data);
	return rc;
}

int react_emit_artifact_event(struct react_event_sink sink, const char *kind, const char *path,
			      const char *source)
{
	cJSON *data;
	int rc;

	if (!react_events_enabled(sink))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "kind", kind ? kind : "");
	cJSON_AddStringToObject(data, "path", path ? path : "");
	if (source)
		cJSON_AddStringToObject(data, "source", source);
	rc = react_emit_event(sink, MORPH_EVENT_ARTIFACT, "artifact.ready", "ready",
			      "artifact ready", data);
	cJSON_Delete(data);
	return rc;
}

void react_emit_artifacts_from_list(struct react_event_sink sink,
				    const struct tool_artifact_list *artifacts, const char *source)
{
	if (!artifacts)
		return;
	for (int i = 0; i < artifacts->count; i++) {
		const struct tool_artifact *artifact = &artifacts->items[i];
		react_emit_artifact_event(sink, tool_artifact_kind_name(artifact->kind),
					  artifact->path, source);
	}
}

int react_emit_hitl_event(struct react_event_sink sink, const char *name, const char *phase,
			  const char *message, const char *tool, const char *args_json,
			  const char *tool_call_id, const char *verdict)
{
	cJSON *data;
	cJSON *args = NULL;
	int rc;

	if (!react_events_enabled(sink))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "tool", tool ? tool : "");
	cJSON_AddStringToObject(data, "tool_call_id", tool_call_id ? tool_call_id : "");
	args = args_json && *args_json ? cJSON_Parse(args_json) : NULL;
	if (!args)
		args = cJSON_CreateObject();
	if (!args) {
		cJSON_Delete(data);
		return -ENOMEM;
	}
	cJSON_AddItemToObject(data, "args", args);
	if (verdict)
		cJSON_AddStringToObject(data, "verdict", verdict);
	rc = react_emit_event(sink, MORPH_EVENT_HITL, name, phase, message, data);
	cJSON_Delete(data);
	return rc;
}
