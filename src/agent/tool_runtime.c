#include "tool_runtime.h"
#include "util/json.h"
#include <stddef.h>
#include <string.h>

int tool_runtime_emit_stream(const struct tool_runtime_context *ctx, const char *tool,
			     const char *kind, const char *text)
{
	struct morph_event ev;
	struct json *data;
	struct json *chunk;
	int rc;

	if (!ctx || !ctx->event_cb || !text || !*text)
		return 0;

	data = json_object();
	if (!data)
		return -ENOMEM;
	json_add_string(data, "tool", tool ? tool : "");
	json_add_string(data, "tool_call_id", ctx->tool_call_id ? ctx->tool_call_id : "");
	json_add_string(data, "kind", kind ? kind : "text");
	/* Internal stream fragments can end between UTF-8 bytes; the receiving
	 * terminal sanitizer retains that boundary state. */
	chunk = json_string_bytes_n(text, strlen(text));
	if (!chunk) {
		json_free(data);
		return -ENOMEM;
	}
	rc = json_object_add(data, "text", chunk);
	if (rc != 0) {
		json_free(chunk);
		json_free(data);
		return rc;
	}

	ev.type = MORPH_EVENT_TOOL;
	ev.name = "tool.stream.delta";
	ev.phase = "delta";
	ev.message = NULL;
	ev.data = data;
	ev.turn_id = ctx->turn_id;
	rc = morph_event_emit(ctx->event_cb, ctx->event_user_data, &ev);
	json_free(data);
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
