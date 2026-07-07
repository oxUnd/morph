#include "tool_runtime.h"
#include "cJSON.h"
#include <stddef.h>

static _Thread_local struct tool_runtime_context current_tool_runtime;
static _Thread_local int current_tool_runtime_set;

void tool_runtime_set_current(const struct tool_runtime_context *ctx)
{
	if (ctx) {
		current_tool_runtime = *ctx;
		current_tool_runtime_set = 1;
	} else {
		current_tool_runtime = (struct tool_runtime_context){0};
		current_tool_runtime_set = 0;
	}
}

const struct tool_runtime_context *tool_runtime_get_current(void)
{
	if (!current_tool_runtime_set)
		return NULL;
	return &current_tool_runtime;
}

int tool_runtime_emit_stream(const char *tool, const char *kind,
			     const char *text)
{
	const struct tool_runtime_context *ctx = tool_runtime_get_current();
	struct morph_event ev;
	cJSON *data;
	int rc;

	if (!ctx || !ctx->event_cb || !text || !*text)
		return 0;

	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "tool", tool ? tool : "");
	cJSON_AddStringToObject(data, "tool_call_id",
				ctx->tool_call_id ? ctx->tool_call_id : "");
	cJSON_AddStringToObject(data, "kind", kind ? kind : "text");
	cJSON_AddStringToObject(data, "text", text);

	ev.type = MORPH_EVENT_TOOL;
	ev.name = "tool.stream.delta";
	ev.phase = "delta";
	ev.message = NULL;
	ev.data = data;
	ev.turn_id = ctx->turn_id;
	rc = morph_event_emit(ctx->event_cb, ctx->event_user_data, &ev);
	cJSON_Delete(data);
	return rc;
}

int tool_runtime_stream_to_buf_cb(const char *token, void *user_data)
{
	struct tool_runtime_stream_sink *sink = user_data;
	const char *text = token ? token : "";
	int rc;

	if (!sink || !sink->buf)
		return -EINVAL;
	rc = morph_buf_puts(sink->buf, text);
	if (rc != 0)
		return rc;
	(void)tool_runtime_emit_stream(sink->tool, sink->kind, text);
	return 0;
}
