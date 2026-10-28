#include "tool_runtime.h"
#include "cJSON.h"
#include <stddef.h>

int tool_runtime_emit_stream(const struct tool_runtime_context *ctx, const char *tool,
			     const char *kind, const char *text)
{
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
	(void)tool_runtime_emit_stream(sink->runtime, sink->tool, sink->kind, text);
	return 0;
}
