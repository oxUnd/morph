#include "llm.h"
#include "agent/tool_runtime.h"
#include "adapter.h"
#include "codec.h"
#include "chat_codec.h"
#include "stream.h"
#include "agent/tool.h"
#include "util/log.h"
#include "util/file.h"
#include "util/utf8.h"
#include "util/arena.h"
#include "util/json.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/image_util.h"
#include "http/client.h"
#include "http/sse.h"
#include "cJSON.h"
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define LLM_IMAGE_QA_MAX_DIM 360

struct model *model_bind_tool_request(const struct model *source,
				      const struct tool_runtime_context *runtime,
				      struct model *storage)
{
	if (!source || !storage)
		return NULL;
	*storage = *source;
	if (runtime)
		storage->http = runtime->http;
	return storage;
}

void model_report_usage(const struct model *model, const struct model_usage *usage)
{
	const struct model_usage_sink *sink = model ? model->usage_sink : NULL;

	if (sink && sink->callback && usage) {
		struct model_usage tagged = *usage;

		snprintf(tagged.phase, sizeof(tagged.phase), "%s",
			 usage->phase[0] ? usage->phase : "turn");
		sink->callback(&tagged, sink->user_data);
	}
}

static void model_clear_last_error(struct model *self)
{
	if (self) {
		self->last_error[0] = '\0';
		memset(&self->error_info, 0, sizeof(self->error_info));
	}
}

static void model_set_last_error(struct model *self, const char *fmt, ...)
{
	va_list ap;

	if (!self || !fmt)
		return;
	va_start(ap, fmt);
	vsnprintf(self->last_error, sizeof(self->last_error), fmt, ap);
	va_end(ap);
}

struct llm_http_ctx {
	struct sse_parser *parser;
	struct arena *arena;
	morph_buf_t error_buf;
};

static int llm_http_cb(const char *data, size_t len, void *ud)
{
	struct llm_http_ctx *hctx = ud;
	if (hctx->parser) {
		int rc = sse_parser_feed(hctx->parser, data, len);
		if (rc != 0)
			return rc;
	}
	(void)morph_buf_append(&hctx->error_buf, data, len);
	return 0;
}

static const char *llm_extract_error(const char *raw, struct arena *arena)
{
	if (!raw || !*raw)
		return NULL;
	cJSON *root = cJSON_Parse(raw);
	if (!root)
		return NULL;
	const char *msg = NULL;
	cJSON *err_obj = cJSON_GetObjectItem(root, "error");
	if (cJSON_IsObject(err_obj)) {
		cJSON *msg_item = cJSON_GetObjectItem(err_obj, "message");
		if (cJSON_IsString(msg_item) && msg_item->valuestring)
			msg = arena_strdup(arena, msg_item->valuestring);
	}
	cJSON_Delete(root);
	return msg;
}

static int llm_retryable_status(int status)
{
	return status == 408 || status == 429 || status == 500 || status == 502 || status == 503 ||
	       status == 504;
}

static int llm_stream_started(const struct llm_stream_ctx *ctx)
{
	return ctx->emitted || ctx->accumulated.len > 0 || ctx->tool_call_count > 0;
}

static int llm_retry_delay_seconds(int retry_index)
{
	int seconds = 1 << (retry_index > 3 ? 3 : retry_index);
	return seconds > 8 ? 8 : seconds;
}

static void llm_record_error(struct model *self, int status, const char *raw, int started,
			     long retry_after_seconds)
{
	struct model_error_info *info = &self->error_info;
	cJSON *json = raw ? cJSON_Parse(raw) : NULL;
	const cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
	const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "code"));
	if (!code)
		code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "type"));
	memset(info, 0, sizeof(*info));
	info->http_status = status > 0 ? status : 0;
	info->output_started = started;
	info->retry_after_seconds = retry_after_seconds;
	if (code)
		snprintf(info->provider_code, sizeof(info->provider_code), "%s", code);
	if (status < 0)
		info->kind = started ? MODEL_ERROR_INCOMPLETE : MODEL_ERROR_NETWORK;
	else if (status == 401 || status == 403)
		info->kind = MODEL_ERROR_AUTH;
	else if (status == 429)
		info->kind = MODEL_ERROR_RATE_LIMIT;
	else if (status >= 500)
		info->kind = MODEL_ERROR_SERVER;
	else if (status >= 400)
		info->kind = MODEL_ERROR_API;
	if (status == 400 || status == 413) {
		if (code &&
		    (!strcmp(code, "context_length_exceeded") ||
		     !strcmp(code, "context_window_exceeded") || !strcmp(code, "prompt_too_long")))
			info->kind = MODEL_ERROR_CONTEXT;
		const char *message =
			cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message"));
		if (message && (strstr(message, "maximum context length") ||
				strstr(message, "exceeds the context window") ||
				strstr(message, "prompt is too long")))
			info->kind = MODEL_ERROR_CONTEXT;
	}
	if (info->kind == MODEL_ERROR_CONTEXT && started)
		info->kind = MODEL_ERROR_INCOMPLETE;
	info->retryable = !started && (status == MORPH_ERR_NETWORK || status == -ETIMEDOUT ||
				       status == -ECONNRESET || status == -ECONNREFUSED ||
				       llm_retryable_status(status));
	if (status == -ECANCELED || status == -EINTR)
		info->retryable = 0;
	cJSON_Delete(json);
}

static int llm_post_sse_with_retry(struct model *self, struct arena *arena, const char *operation,
				   const char *usage_phase, const char *url, const char *body,
				   size_t body_len, const char **headers, long timeout,
				   sse_callback cb, llm_stream_callback stream_cb,
				   sse_event_cb event_cb, void *user_data,
				   struct llm_stream_ctx *result, const char **error_detail)
{
	int retries = self->retry_count;
	if (retries < 0)
		retries = 0;
	else if (retries > 10)
		retries = 10;
	if (error_detail)
		*error_detail = NULL;

	for (int attempt = 0;; attempt++) {
		struct llm_stream_ctx ctx;
		if (stream_cb)
			llm_stream_init_typed(&ctx, arena, stream_cb, user_data);
		else
			llm_stream_init(&ctx, arena, cb, user_data);

		ctx.logger = self->http.logger;
		snprintf(ctx.usage.phase, sizeof(ctx.usage.phase), "%s",
			 usage_phase ? usage_phase : "turn");
		struct sse_parser parser;
		sse_parser_init(&parser, event_cb ? event_cb : llm_chat_decode_event, &ctx);
		struct llm_http_ctx hctx = {
			.parser = &parser,
			.arena = arena,
		};
		if (morph_buf_init_arena(&hctx.error_buf, arena, 4096) != 0)
			memset(&hctx.error_buf, 0, sizeof(hctx.error_buf));

		struct http_sse_metadata metadata = {0};
		int status = http_post_sse_ex_timeout_metadata(
			&self->http, url, body, body_len, "application/json", headers, 1, timeout,
			llm_http_cb, &hctx, &metadata);
		sse_parser_free(&parser);

		const char *detail = NULL;
		if (status >= 400 && hctx.error_buf.len > 0)
			detail = llm_extract_error(hctx.error_buf.data, arena);
		if (error_detail)
			*error_detail = detail;

		llm_record_error(self, status, hctx.error_buf.data, llm_stream_started(&ctx),
				 metadata.retry_after_seconds);
		if (self->error_info.kind == MODEL_ERROR_CONTEXT) {
			*result = ctx;
			return MORPH_ERR_CONTEXT_LIMIT;
		}
		if (self->error_info.retryable && attempt < retries) {
			long delay = self->error_info.retry_after_seconds > 0
					     ? self->error_info.retry_after_seconds
					     : llm_retry_delay_seconds(attempt);
			if (detail)
				log_warn(self ? self->http.logger : NULL,
					 "%s: HTTP %d (%s), retry %d/%d in %lds", operation, status,
					 detail, attempt + 1, retries, delay);
			else
				log_warn(self ? self->http.logger : NULL,
					 "%s: HTTP %d, retry %d/%d in %lds", operation, status,
					 attempt + 1, retries, delay);
			int wait_rc = http_wait_cancelable(&self->http, (unsigned int)delay * 1000);
			if (wait_rc < 0)
				return wait_rc;
			continue;
		}

		*result = ctx;
		return status;
	}
}

static int llm_chat(struct model *self, struct arena *arena, const char *system_prompt,
		    const char **messages, int n, const struct model_chat_options *opts,
		    sse_callback cb, void *user_data)
{
	int max_tokens;
	long timeout;

	if (!self || !self->api_key[0]) {
		log_err(self ? self->http.logger : NULL, "llm_chat: no API key configured");
		if (self) {
			model_clear_last_error(self);
			self->error_info.kind = MODEL_ERROR_CONFIG;
		}
		return MORPH_TRACE_ERROR(self ? self->http.logger : NULL, MORPH_ERR_NOT_CONFIGURED);
	}

	log_dbg(self ? self->http.logger : NULL, "llm_chat: start, model=%s, api_base=%s",
		self->model_id, self->api_base);
	max_tokens = opts && opts->max_tokens > 0 ? opts->max_tokens : self->max_tokens;
	timeout = opts && opts->timeout_seconds > 0
			  ? opts->timeout_seconds
			  : (self->timeout_seconds > 0 ? self->timeout_seconds : 300L);

	char *body = NULL;
	size_t body_len = 0;
	int rc = llm_chat_encode_body(self ? self->http.logger : NULL, arena, self->model_id,
				      system_prompt, messages, n, max_tokens, 1,
				      self->extra_body_json, llm_adapter_resolve(self),
				      opts ? opts->disable_thinking : 0, &body, &body_len);
	if (rc < 0)
		return rc;

	size_t clean_len = utf8_sanitize_into(body, body, body_len);
	body[clean_len] = '\0';
	body_len = clean_len;

	char url[512];
	snprintf(url, sizeof(url), "%s/chat/completions", self->api_base);

	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", self->api_key);
	const char *extra_headers[] = {auth_header};

	log_dbg(self ? self->http.logger : NULL,
		"llm_chat: sending SSE request to %s, timeout=%lds", url, timeout);
	struct llm_stream_ctx ctx;
	const char *detail = NULL;
	int status = llm_post_sse_with_retry(
		self, arena, "llm_chat", opts ? opts->usage_phase : NULL, url, body, body_len,
		extra_headers, timeout, cb, NULL, NULL, user_data, &ctx, &detail);
	log_dbg(self ? self->http.logger : NULL, "llm_chat: SSE request done, status=%d", status);

	if (status < 0) {
		model_set_last_error(self, "%s", detail ? detail : morph_strerror(status));
		log_err(self ? self->http.logger : NULL, "llm_chat: SSE request failed: %s",
			morph_strerror(status));
		return status;
	}
	if (status >= 400) {
		if (detail)
			log_err(self ? self->http.logger : NULL,
				"llm_chat: API returned HTTP %d: %s", status, detail);
		else
			log_err(self ? self->http.logger : NULL, "llm_chat: API returned HTTP %d",
				status);
		return MORPH_TRACE_ERROR(self ? self->http.logger : NULL, MORPH_ERR_API);
	}

	llm_usage_report(self, &ctx, body_len);
	if (opts && opts->usage)
		*opts->usage = ctx.usage;
	return status;
}

static int add_system_message(cJSON *messages, const char *system_prompt)
{
	if (!messages || !system_prompt || !*system_prompt)
		return 0;
	cJSON *msg = cJSON_CreateObject();
	if (!msg)
		return -ENOMEM;
	cJSON_AddStringToObject(msg, "role", "system");
	cJSON_AddStringToObject(msg, "content", system_prompt);
	cJSON_AddItemToArray(messages, msg);
	return 0;
}

static int llm_chat_with_image(struct model *self, struct arena *arena, const char *system_prompt,
			       const char *prompt, const char *image_path,
			       const struct model_image_chat_options *opts, sse_callback cb,
			       void *user_data)
{
	int max_tokens;
	int max_dim;
	long timeout;

	model_clear_last_error(self);
	if (!self || !self->api_key[0]) {
		log_err(self ? self->http.logger : NULL,
			"llm_chat_with_image: no API key configured");
		if (self) {
			model_clear_last_error(self);
			self->error_info.kind = MODEL_ERROR_CONFIG;
		}
		return MORPH_TRACE_ERROR(self ? self->http.logger : NULL, MORPH_ERR_NOT_CONFIGURED);
	}
	if (!arena || !prompt || !*prompt || !image_path || !*image_path)
		return -EINVAL;

	max_tokens = opts && opts->max_tokens > 0
			     ? opts->max_tokens
			     : (self->max_tokens > 0 ? self->max_tokens : 4096);
	max_dim = opts && opts->max_dim > 0 ? opts->max_dim : LLM_IMAGE_QA_MAX_DIM;
	timeout = opts && opts->timeout_seconds > 0
			  ? opts->timeout_seconds
			  : (self->timeout_seconds > 0 ? self->timeout_seconds : 300L);

	char *data_uri = NULL;
	int encode_rc = image_encode_data_uri(image_path, max_dim, &data_uri);
	if (encode_rc < 0) {
		log_err(self ? self->http.logger : NULL,
			"llm_chat_with_image: failed to encode image: %s", image_path);
		model_set_last_error(self, "failed to read or encode image: %s", image_path);
		return MORPH_TRACE_ERROR(self ? self->http.logger : NULL, encode_rc);
	}

	cJSON *root = cJSON_CreateObject();
	cJSON *messages = cJSON_CreateArray();
	if (!root || !messages) {
		cJSON_Delete(root);
		cJSON_Delete(messages);
		free(data_uri);
		return -ENOMEM;
	}
	cJSON_AddStringToObject(root, "model", self->model_id);
	cJSON_AddItemToObject(root, "messages", messages);
	if (add_system_message(messages, system_prompt) < 0) {
		cJSON_Delete(root);
		free(data_uri);
		return -ENOMEM;
	}

	cJSON *user = cJSON_CreateObject();
	cJSON *content = cJSON_CreateArray();
	cJSON *text = cJSON_CreateObject();
	cJSON *image = cJSON_CreateObject();
	cJSON *image_url = cJSON_CreateObject();
	if (!user || !content || !text || !image || !image_url) {
		cJSON_Delete(user);
		cJSON_Delete(content);
		cJSON_Delete(text);
		cJSON_Delete(image);
		cJSON_Delete(image_url);
		cJSON_Delete(root);
		free(data_uri);
		return -ENOMEM;
	}
	cJSON_AddStringToObject(user, "role", "user");
	cJSON_AddItemToObject(user, "content", content);
	cJSON_AddStringToObject(text, "type", "text");
	cJSON_AddStringToObject(text, "text", prompt);
	cJSON_AddItemToArray(content, text);

	cJSON_AddStringToObject(image, "type", "image_url");
	cJSON *uri_value = morph_json_take_string(data_uri);
	if (!uri_value || !cJSON_AddItemToObject(image_url, "url", uri_value)) {
		cJSON_Delete(uri_value);
		cJSON_Delete(image_url);
		cJSON_Delete(image);
		cJSON_Delete(user);
		cJSON_Delete(root);
		return MORPH_TRACE_ERROR(self ? self->http.logger : NULL, -ENOMEM);
	}
	cJSON_AddItemToObject(image, "image_url", image_url);
	cJSON_AddItemToArray(content, image);
	cJSON_AddItemToArray(messages, user);
	cJSON_AddBoolToObject(root, "stream", 1);
	{
		cJSON *stream_opts = cJSON_CreateObject();
		if (stream_opts) {
			cJSON_AddBoolToObject(stream_opts, "include_usage", 1);
			cJSON_AddItemToObject(root, "stream_options", stream_opts);
		}
	}
	cJSON_AddNumberToObject(root, "max_tokens", max_tokens);
	int extra_rc =
		llm_codec_add_options(self ? self->http.logger : NULL, root, self->extra_body_json);
	if (extra_rc < 0) {
		cJSON_Delete(root);
		return extra_rc;
	}

	char *body = morph_json_print(arena, root);
	cJSON_Delete(root);
	if (!body)
		return -ENOMEM;

	size_t body_len = strlen(body);
	size_t clean_len = utf8_sanitize_into(body, body, body_len);
	body[clean_len] = '\0';
	body_len = clean_len;

	char url[512];
	snprintf(url, sizeof(url), "%s/chat/completions", self->api_base);

	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", self->api_key);
	const char *extra_headers[] = {auth_header};

	log_dbg(self ? self->http.logger : NULL,
		"llm_chat_with_image: sending SSE request to %s, timeout=%lds, max_tokens=%d, "
		"max_dim=%d, body=%zu",
		url, timeout, max_tokens, max_dim, body_len);
	struct llm_stream_ctx ctx;
	const char *detail = NULL;
	int status = llm_post_sse_with_retry(self, arena, "llm_chat_with_image", NULL, url, body,
					     body_len, extra_headers, timeout, cb, NULL, NULL,
					     user_data, &ctx, &detail);
	log_dbg(self ? self->http.logger : NULL, "llm_chat_with_image: SSE request done, status=%d",
		status);

	if (status < 0) {
		log_err(self ? self->http.logger : NULL,
			"llm_chat_with_image: SSE request failed: %d", status);
		model_set_last_error(self, "HTTP/SSE request failed: %s", morph_strerror(status));
		return status;
	}
	if (status >= 400) {
		if (detail) {
			log_err(self ? self->http.logger : NULL,
				"llm_chat_with_image: API returned HTTP %d: %s", status, detail);
			model_set_last_error(self, "HTTP %d: %s", status, detail);
		} else {
			log_err(self ? self->http.logger : NULL,
				"llm_chat_with_image: API returned HTTP %d", status);
			model_set_last_error(self, "HTTP %d from %s", status, self->api_base);
		}
		return MORPH_TRACE_ERROR(self ? self->http.logger : NULL, MORPH_ERR_API);
	}

	llm_usage_report(self, &ctx, body_len);
	return status;
}

/* Count transmitted text, not JSON escaping around message contents. Framing
 * remains an estimate; the caller reserves a separate protocol safety margin. */
static int64_t input_text_tokens(const cJSON *value, model_token_counter count, void *user_data)
{
	int64_t total = 0;
	const cJSON *child;

	if (cJSON_IsString(value))
		return count(value->valuestring, user_data);
	if (cJSON_IsObject(value))
		total = 12;
	cJSON_ArrayForEach (child, value)
		total += input_text_tokens(child, count, user_data);
	return total;
}

int model_input_snapshot(const struct model *model, const char *system_prompt,
			 const struct chat_message *messages, int msg_count,
			 const struct tool_desc *tools, int tool_count, cJSON **out)
{
	const struct llm_codec *codec = llm_codec_resolve(model);
	cJSON *root;
	cJSON *input;
	cJSON *definitions;

	if (!out || msg_count < 0 || tool_count < 0 || (msg_count && !messages) ||
	    (tool_count && !tools))
		return MORPH_TRACE_ERROR(model ? model->http.logger : NULL, -EINVAL);
	*out = NULL;
	root = cJSON_CreateObject();
	input = codec->input(model, system_prompt, messages, msg_count);
	definitions = codec->tools(tools, tool_count);
	if (!root || !input || !definitions) {
		cJSON_Delete(root);
		cJSON_Delete(input);
		cJSON_Delete(definitions);
		return MORPH_TRACE_ERROR(model ? model->http.logger : NULL, -ENOMEM);
	}
	if (!cJSON_AddItemToObject(root, codec->input_key, input)) {
		cJSON_Delete(input);
		cJSON_Delete(definitions);
		cJSON_Delete(root);
		return MORPH_TRACE_ERROR(model ? model->http.logger : NULL, -ENOMEM);
	}
	if (!cJSON_AddItemToObject(root, "tools", definitions)) {
		cJSON_Delete(definitions);
		cJSON_Delete(root);
		return MORPH_TRACE_ERROR(model ? model->http.logger : NULL, -ENOMEM);
	}
	if (codec->system_key && system_prompt && *system_prompt &&
	    !cJSON_AddStringToObject(root, codec->system_key, system_prompt)) {
		cJSON_Delete(root);
		return MORPH_TRACE_ERROR(model ? model->http.logger : NULL, -ENOMEM);
	}
	*out = root;
	return 0;
}

int model_estimate_input_tokens(const struct model *model, const char *system_prompt,
				const struct chat_message *messages, int msg_count,
				const struct tool_desc *tools, int tool_count,
				model_token_counter count, void *user_data, int *tokens)
{
	const struct llm_codec *codec = llm_codec_resolve(model);
	cJSON *input;
	cJSON *wire_tools;
	char *json;
	int64_t total;

	if (!count || !tokens || msg_count < 0 || tool_count < 0 || (msg_count && !messages) ||
	    (tool_count && !tools))
		return MORPH_TRACE_ERROR(model ? model->http.logger : NULL, -EINVAL);
	input = codec->input(model, system_prompt, messages, msg_count);
	if (!input)
		return MORPH_TRACE_ERROR(model ? model->http.logger : NULL, -ENOMEM);
	total = 16 + input_text_tokens(input, count, user_data);
	cJSON_Delete(input);
	if (codec->system_key && system_prompt && system_prompt[0])
		total += count(system_prompt, user_data);
	if (tool_count > 0) {
		wire_tools = codec->tools(tools, tool_count);
		if (!wire_tools)
			return MORPH_TRACE_ERROR(model ? model->http.logger : NULL, -ENOMEM);
		/* Schema keys and constraints are part of the model-visible definition. */
		json = cJSON_PrintUnformatted(wire_tools);
		cJSON_Delete(wire_tools);
		if (!json)
			return MORPH_TRACE_ERROR(model ? model->http.logger : NULL, -ENOMEM);
		total += count(json, user_data);
		free(json);
	}
	*tokens = total > INT_MAX ? INT_MAX : (int)total;
	return 0;
}

static int llm_chat_with_tools_impl(struct model *self, struct arena *arena,
				    const char *system_prompt, struct chat_message *messages,
				    int msg_count, struct tool_desc *tools, int tool_count,
				    struct chat_response *response, sse_callback thought_cb,
				    llm_stream_callback stream_cb, void *stream_ud)
{
	const struct llm_codec *codec = llm_codec_resolve(self);
	const struct llm_adapter *adapter = llm_adapter_resolve(self);
	struct llm_stream_ctx ctx;
	cJSON *root = NULL;
	char *body;
	char url[512];
	char authorization[512];
	const char *headers[] = {authorization};
	const char *detail = NULL;
	long timeout;
	int rc;
	int status;

	if (!self || !arena || !response)
		return -EINVAL;
	model_clear_last_error(self);
	if (!self->api_key[0]) {
		self->error_info.kind = MODEL_ERROR_CONFIG;
		model_set_last_error(self, "no API key configured");
		return MORPH_ERR_NOT_CONFIGURED;
	}
	memset(response, 0, sizeof(*response));
	response->arena = arena;
	rc = model_input_snapshot(self, system_prompt, messages, msg_count, tools, tool_count,
				  &root);
	if (rc != 0)
		return rc;
	cJSON_AddStringToObject(root, "model", self->model_id);
	cJSON_AddBoolToObject(root, "stream", 1);
	codec->configure(self, root, tool_count);
	rc = llm_codec_add_options(self->http.logger, root, self->extra_body_json);
	if (rc != 0) {
		cJSON_Delete(root);
		return rc;
	}
	body = morph_json_print(arena, root);
	cJSON_Delete(root);
	if (!body)
		return -ENOMEM;
	body[utf8_sanitize_into(body, body, strlen(body))] = '\0';
	snprintf(url, sizeof(url), "%s%s", self->api_base, codec->endpoint);
	snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", self->api_key);
	timeout = self->timeout_seconds > 0 ? self->timeout_seconds : 300L;
	status = llm_post_sse_with_retry(self, arena, "llm_chat_with_tools", NULL, url, body,
					 strlen(body), headers, timeout, thought_cb, stream_cb,
					 codec->event, stream_ud, &ctx, &detail);
	if (status < 0) {
		model_set_last_error(self, "HTTP/SSE request failed: %s", morph_strerror(status));
		return status;
	}
	if (status >= 400) {
		model_set_last_error(self, "HTTP %d: %s", status,
				     detail ? detail : "model request failed");
		response->content = (char *)detail;
		return MORPH_ERR_API;
	}
	response->content = ctx.accumulated.len > 0 ? ctx.accumulated.data : NULL;
	if (adapter->capture_reasoning && ctx.reasoning.len > 0)
		response->reasoning_content = ctx.reasoning.data;
	llm_stream_transfer_tool_calls(&ctx, response);
	llm_usage_finalize(self, &ctx, strlen(body));
	response->usage = ctx.usage;
	model_report_usage(self, &response->usage);
	return status;
}

static int llm_chat_with_tools(struct model *self, struct arena *arena, const char *system_prompt,
			       struct chat_message *messages, int msg_count,
			       struct tool_desc *tools, int tool_count,
			       struct chat_response *response, sse_callback thought_cb,
			       void *thought_ud)
{
	return llm_chat_with_tools_impl(self, arena, system_prompt, messages, msg_count, tools,
					tool_count, response, thought_cb, NULL, thought_ud);
}

static int llm_chat_with_tools_stream(struct model *self, struct arena *arena,
				      const char *system_prompt, struct chat_message *messages,
				      int msg_count, struct tool_desc *tools, int tool_count,
				      struct chat_response *response, llm_stream_callback stream_cb,
				      void *stream_ud)
{
	return llm_chat_with_tools_impl(self, arena, system_prompt, messages, msg_count, tools,
					tool_count, response, NULL, stream_cb, stream_ud);
}

static int llm_generate(struct model *self, const char *prompt, const char *out_path)
{
	if (!self || !prompt)
		return -EINVAL;

	struct arena *arena = arena_create(8192);
	if (!arena)
		return -ENOMEM;

	const char *messages[] = {prompt};
	char *body = NULL;
	size_t body_len = 0;
	int rc = llm_chat_encode_body(self ? self->http.logger : NULL, arena, self->model_id, NULL,
				      messages, 1, self->max_tokens, 0, self->extra_body_json, NULL,
				      0, &body, &body_len);
	if (rc < 0) {
		arena_destroy(arena);
		return rc;
	}

	struct http_response resp = {0};
	char url[512];
	snprintf(url, sizeof(url), "%s/chat/completions", self->api_base);
	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", self->api_key);
	const char *extra_headers[] = {auth_header};
	rc = http_post_ex(&self->http, url, body, body_len, "application/json", extra_headers, 1,
			  &resp);
	arena_destroy(arena);

	if (rc < 0) {
		http_response_free(&resp);
		return rc;
	}

	if (resp.status_code != 200) {
		log_err(self ? self->http.logger : NULL, "llm_generate: API returned %d",
			resp.status_code);
		http_response_free(&resp);
		return MORPH_TRACE_ERROR(self ? self->http.logger : NULL, MORPH_ERR_API);
	}

	rc = 0;
	if (out_path && resp.body.data)
		rc = file_write_all(out_path, resp.body.data, resp.body.len);

	http_response_free(&resp);
	return rc;
}

static void llm_destroy(struct model *self)
{
	if (self)
		free(self);
}

struct model *model_llm_create(struct morph_log *logger, const char *provider, const char *model_id,
			       const char *api_base, const char *api_key)
{
	struct model *m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	m->http.logger = logger;
	strncpy(m->provider, provider ? provider : "openai", sizeof(m->provider) - 1);
	strncpy(m->adapter, llm_adapter_infer(provider), sizeof(m->adapter) - 1);
	strncpy(m->model_id, model_id ? model_id : "gpt-4o", sizeof(m->model_id) - 1);
	if (api_base)
		strncpy(m->api_base, api_base, sizeof(m->api_base) - 1);
	else
		strncpy(m->api_base, "https://api.openai.com/v1", sizeof(m->api_base) - 1);
	if (api_key)
		strncpy(m->api_key, api_key, sizeof(m->api_key) - 1);
	m->context_limit = 128000;
	m->max_tokens = 4096;
	m->timeout_seconds = 0;
	m->retry_count = 3;
	m->chat = llm_chat;
	m->chat_with_tools = llm_chat_with_tools;
	m->chat_with_tools_stream = llm_chat_with_tools_stream;
	m->chat_with_image = llm_chat_with_image;
	m->generate = llm_generate;
	m->destroy = llm_destroy;
	return m;
}

struct model *model_llm_clone(const struct model *source, struct morph_log *logger)
{
	struct model *copy;

	if (!source)
		return NULL;
	copy = model_llm_create(logger, source->provider, source->model_id, source->api_base,
				source->api_key);
	if (!copy)
		return NULL;
	memcpy(copy->name, source->name, sizeof(copy->name));
	memcpy(copy->adapter, source->adapter, sizeof(copy->adapter));
	memcpy(copy->extra_body_json, source->extra_body_json, sizeof(copy->extra_body_json));
	copy->context_limit = source->context_limit;
	copy->max_tokens = source->max_tokens;
	copy->timeout_seconds = source->timeout_seconds;
	copy->retry_count = source->retry_count;
	copy->poll_interval_seconds = source->poll_interval_seconds;
	copy->poll_timeout_seconds = source->poll_timeout_seconds;
	return copy;
}

void model_destroy(struct model *m)
{
	if (m && m->destroy)
		m->destroy(m);
}

void chat_response_free(struct chat_response *resp)
{
	if (!resp)
		return;
	if (resp->arena) {
		memset(resp, 0, sizeof(*resp));
		return;
	}
	free(resp->content);
	resp->content = NULL;
	free(resp->reasoning_content);
	resp->reasoning_content = NULL;
	for (int i = 0; i < resp->tool_call_count; i++)
		free(resp->tool_calls[i].arguments);
	free(resp->tool_calls);
	resp->tool_calls = NULL;
	resp->tool_call_count = 0;
}

void chat_message_cleanup(struct chat_message *msg, struct arena *arena)
{
	if (!msg)
		return;
	if (arena) {
		memset(msg, 0, sizeof(*msg));
		return;
	}
	free(msg->role);
	free(msg->content);
	free(msg->tool_call_id);
	free(msg->reasoning_content);
	for (int i = 0; i < msg->tool_call_count; i++)
		free(msg->tool_calls[i].arguments);
	free(msg->tool_calls);
	memset(msg, 0, sizeof(*msg));
}

void tool_call_cleanup(struct tool_call *tc, struct arena *arena)
{
	if (!tc)
		return;
	if (arena) {
		memset(tc, 0, sizeof(*tc));
		return;
	}
	free(tc->arguments);
	tc->arguments = NULL;
}
