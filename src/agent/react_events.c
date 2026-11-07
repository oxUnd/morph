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
		     const char *phase, const char *message, struct json *data)
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
	struct json *data;
	int rc;

	if (!react_events_enabled(sink))
		return 0;
	data = json_object();
	if (!data)
		return -ENOMEM;
	json_add_string(data, "text", text ? text : "");
	rc = react_emit_event(sink, type, name, phase, message, data);
	json_free(data);
	return rc;
}

int react_emit_observation_event(struct react_event_sink sink, const char *text, const char *tool,
				 int error_code, const struct tool_artifact_list *artifacts,
				 const struct json *result_data, const struct json *result_ui)
{
	struct json *data;
	struct json *artifact_json = NULL;
	struct json *data_copy = NULL;
	struct json *ui_copy = NULL;
	int rc;

	if (!react_events_enabled(sink))
		return 0;
	data = json_object();
	if (!data)
		return -ENOMEM;
	json_add_string(data, "text", text ? text : "");
	if (tool)
		json_add_string(data, "tool", tool);
	if (error_code < 0) {
		json_add_number(data, "error_code", error_code);
		json_add_string(data, "error", morph_strerror(error_code));
	}
	if (artifacts && artifacts->count > 0) {
		artifact_json = tool_artifact_list_to_json(artifacts);
		if (artifact_json) {
			rc = json_object_add(data, "artifacts", artifact_json);
			if (rc != 0)
				goto out_free_values;
		}
	}
	if (result_data) {
		data_copy = json_clone(result_data, 1);
		if (data_copy) {
			rc = json_object_add(data, "data", data_copy);
			if (rc != 0)
				goto out_free_values;
		}
	}
	if (result_ui) {
		ui_copy = json_clone(result_ui, 1);
		if (ui_copy) {
			rc = json_object_add(data, "ui", ui_copy);
			if (rc != 0)
				goto out_free_values;
		}
	}
	rc = react_emit_event(sink, MORPH_EVENT_REACT, "react.observation",
			      error_code < 0 ? "failed" : "end", "tool observation", data);
out_free_values:
	json_free(artifact_json);
	json_free(data_copy);
	json_free(ui_copy);
	json_free(data);
	return rc;
}

int react_emit_auth_required(struct react_event_sink sink, const char *backend,
			     const char *provider, const char *model, const char *tool,
			     const char *reason)
{
	struct json *data;
	int rc;

	if (!react_events_enabled(sink))
		return 0;
	data = json_object();
	if (!data)
		return -ENOMEM;
	json_add_string(data, "kind", "auth_required");
	json_add_string(data, "backend", backend ? backend : "");
	json_add_string(data, "provider", provider ? provider : "");
	json_add_string(data, "model", model ? model : "");
	json_add_string(data, "env_name", "");
	json_add_string(data, "tool", tool ? tool : "");
	json_add_string(data, "reason", reason ? reason : "missing_api_key");
	json_add_bool(data, "retryable", 1);
	rc = react_emit_event(sink, MORPH_EVENT_HITL, "auth.required", "blocked",
			      "authentication required", data);
	json_free(data);
	return rc;
}

int react_emit_tool_event(struct react_event_sink sink, const char *name, const char *phase,
			  const char *message, const char *tool, const char *args_json,
			  const char *tool_call_id, const char *result, int error_code)
{
	struct json *data;
	struct json *args = NULL;
	int text_input = 0;
	int rc;

	if (!react_events_enabled(sink))
		return 0;
	data = json_object();
	if (!data)
		return -ENOMEM;
	json_add_string(data, "tool", tool ? tool : "");
	if (sink.tools && tool) {
		tool_registry_lock(sink.tools);
		struct tool_entry *entry = tool_lookup(sink.tools, tool);

		if (entry && entry->desc.title[0])
			json_add_string(data, "toolTitle", entry->desc.title);
		if (entry && entry->desc.input_kind == TOOL_INPUT_TEXT)
			text_input = 1;
		tool_registry_unlock(sink.tools);
	}
	json_add_string(data, "tool_call_id", tool_call_id ? tool_call_id : "");
	if (text_input) {
		args = json_object();
		if (args)
			json_add_string(args, "input", args_json ? args_json : "");
	} else {
		args = args_json && *args_json ? json_parse_prefix(args_json, NULL, NULL) : NULL;
	}
	if (!args)
		args = json_object();
	if (!args) {
		json_free(data);
		return -ENOMEM;
	}
	rc = json_object_add(data, "args", args);
	if (rc != 0) {
		json_free(args);
		json_free(data);
		return rc;
	}
	if (result)
		json_add_string(data, "result", result);
	if (error_code < 0) {
		const char *error = morph_strerror(error_code);

		if (result && result[0]) {
			static const char prefix[] = "tool error: ";

			error = strncmp(result, prefix, sizeof(prefix) - 1) == 0
					? result + sizeof(prefix) - 1
					: result;
		}
		json_add_number(data, "error_code", error_code);
		json_add_string(data, "error", error);
	}
	rc = react_emit_event(sink, MORPH_EVENT_TOOL, name, phase, message, data);
	json_free(data);
	return rc;
}

int react_emit_artifact_event(struct react_event_sink sink, const char *kind, const char *path,
			      const char *source)
{
	struct json *data;
	int rc;

	if (!react_events_enabled(sink))
		return 0;
	data = json_object();
	if (!data)
		return -ENOMEM;
	json_add_string(data, "kind", kind ? kind : "");
	json_add_string(data, "path", path ? path : "");
	if (source)
		json_add_string(data, "source", source);
	rc = react_emit_event(sink, MORPH_EVENT_ARTIFACT, "artifact.ready", "ready",
			      "artifact ready", data);
	json_free(data);
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
	struct json *data;
	struct json *args = NULL;
	int rc;

	if (!react_events_enabled(sink))
		return 0;
	data = json_object();
	if (!data)
		return -ENOMEM;
	json_add_string(data, "tool", tool ? tool : "");
	json_add_string(data, "tool_call_id", tool_call_id ? tool_call_id : "");
	args = args_json && *args_json ? json_parse_prefix(args_json, NULL, NULL) : NULL;
	if (!args)
		args = json_object();
	if (!args) {
		json_free(data);
		return -ENOMEM;
	}
	rc = json_object_add(data, "args", args);
	if (rc != 0) {
		json_free(args);
		json_free(data);
		return rc;
	}
	if (verdict)
		json_add_string(data, "verdict", verdict);
	rc = react_emit_event(sink, MORPH_EVENT_HITL, name, phase, message, data);
	json_free(data);
	return rc;
}
