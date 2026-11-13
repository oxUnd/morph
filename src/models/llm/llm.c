#include "llm.h"
#include "adapter.h"
#include "codec.h"
#include "chat_codec.h"
#include "stream.h"
#include "agent/tool.h"
#include "nb/log.h"
#include "nb/file.h"
#include "nb/utf8.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "media/image.h"
#include "http/client.h"
#include "http/sse.h"
#include "cJSON.h"
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define LLM_IMAGE_QA_MAX_DIM 2048

const char *model_api_key_borrow(const struct model *model)
{
	return model ? (model->api_key_owned ? model->api_key_owned : model->api_key) : "";
}

int model_authorization_header_create(const struct model *model, nb_buf *header)
{
	nb_buf candidate NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *key = model_api_key_borrow(model);
	int rc;

	if (!model || !header)
		return NB_ERR_INVAL;
	if (header->ptr)
		return NB_ERR_EXISTS;
	if (strpbrk(key, "\r\n"))
		return NB_ERR_INVAL;
	rc = (nb_buf_printf(&candidate, "Authorization: Bearer %s", key) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	(nb_buf_destroy(header), nb_buf_move(header, &candidate));
	return 0;
}

struct model *model_request_view_init(const struct model *source,
				      const struct http_control *control, struct model *storage)
{
	if (!source || !storage)
		return NULL;
	*storage = *source;
	if (control)
		storage->http = *control;
	return storage;
}

void model_report_usage(const struct model *model, const struct model_usage *usage)
{
	const struct model_usage_sink *sink = model ? model->usage_sink : NULL;
	if (model && model->budget_sink && model->budget_sink->callback && usage)
		model->budget_sink->callback(usage, model->budget_sink->user_data);

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
	nb_buf error_buf;
};

static void llm_http_cleanup(struct llm_http_ctx *ctx)
{
	nb_buf_cleanup(&ctx->error_buf);
}

static void llm_stream_move(struct llm_stream_ctx *destination, struct llm_stream_ctx *source)
{
	*destination = *source;
	memset(source, 0, sizeof(*source));
}

static int llm_http_cb(const char *data, size_t len, void *ud)
{
	struct llm_http_ctx *hctx = ud;
	if (hctx->parser) {
		int rc = sse_parser_feed(hctx->parser, data, len);
		if (rc != 0)
			return rc;
	}
	(void)nb_buf_append(&hctx->error_buf, data, len);
	return 0;
}

static char *llm_extract_error(const char *raw)
{
	if (!raw || !*raw)
		return NULL;
	cJSON *root = cJSON_Parse(raw);
	if (!root)
		return NULL;
	char *msg = NULL;
	cJSON *err_obj = cJSON_GetObjectItem(root, "error");
	if (cJSON_IsObject(err_obj)) {
		cJSON *msg_item = cJSON_GetObjectItem(err_obj, "message");
		if (cJSON_IsString(msg_item) && msg_item->valuestring)
			msg = strdup(msg_item->valuestring);
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
	info->retryable =
		!started && (status == MORPH_ERR_NETWORK || status == NB_ERR_TIMEOUT ||
			     status == NB_ERR_CONNECTION_RESET ||
			     status == NB_ERR_CONNECTION_REFUSED || llm_retryable_status(status));
	if (status == NB_ERR_CANCELLED || status == NB_ERR_INTERRUPTED)
		info->retryable = 0;
	cJSON_Delete(json);
}

static int llm_post_sse_with_retry(struct model *self, const char *operation,
				   const char *usage_phase, const char *url, const char *body,
				   size_t body_len, const char **headers, long timeout,
				   sse_callback cb, llm_stream_callback stream_cb,
				   sse_event_cb event_cb, int requires_terminal_event,
				   void *user_data,
				   struct llm_stream_ctx *result, char **error_detail)
{
	int retries = self->retry_count;
	if (retries < 0)
		retries = 0;
	else if (retries > 10)
		retries = 10;
	if (error_detail)
		*error_detail = NULL;

	for (int attempt = 0;; attempt++) {
		struct llm_stream_ctx ctx NB_CLEANUP(llm_stream_cleanup) = {0};
		if (stream_cb)
			llm_stream_init_typed(&ctx, stream_cb, user_data);
		else
			llm_stream_init(&ctx, cb, user_data);

		ctx.logger = self->http.logger;
		snprintf(ctx.usage.phase, sizeof(ctx.usage.phase), "%s",
			 usage_phase ? usage_phase : "turn");
		struct sse_parser parser;
		sse_parser_init(&parser, event_cb ? event_cb : llm_chat_decode_event, &ctx);
		struct llm_http_ctx hctx NB_CLEANUP(llm_http_cleanup) = {
			.parser = &parser,
		};
		int buffer_rc = (nb_buf_init(&hctx.error_buf),
				 nb_buf_reserve(&hctx.error_buf, 4096) ? 0 : NB_ERR_NOMEM);
		if (buffer_rc != 0 || nb_buf_status(&ctx.accumulated) ||
		    nb_buf_status(&ctx.reasoning)) {
			sse_parser_free(&parser);
			return buffer_rc			 ? buffer_rc
			       : nb_buf_status(&ctx.accumulated) ? nb_buf_status(&ctx.accumulated)
								 : nb_buf_status(&ctx.reasoning);
		}

		struct http_sse_metadata metadata = {0};
		int status = http_post_sse_ex_timeout_metadata(
			&self->http, url, body, body_len, "application/json", headers, 1, timeout,
			llm_http_cb, &hctx, &metadata);
		int http_status = status;
		sse_parser_free(&parser);

		char *detail NB_CLEANUP(nb_charp_cleanup) = NULL;
		if (status >= 400 && hctx.error_buf.len > 0)
			detail = llm_extract_error(hctx.error_buf.ptr);
		if (status >= 0 && status < 400 && requires_terminal_event) {
			if (!ctx.terminal_received) {
				ctx.terminal_error = NB_ERR_PROTOCOL;
				int rc = llm_stream_string_copy(
					&ctx.terminal_detail,
					"Model stream closed before a terminal response event.");
				if (rc != 0)
					ctx.terminal_error = rc;
			}
			if (ctx.terminal_error) {
				status = ctx.terminal_error;
				free(detail);
				detail = nb_charp_steal(&ctx.terminal_detail);
			}
		}

		llm_record_error(self, status, hctx.error_buf.ptr, llm_stream_started(&ctx),
				 metadata.retry_after_seconds);
		if (requires_terminal_event && ctx.terminal_error) {
			self->error_info.kind = ctx.terminal_error == MORPH_ERR_API
				? MODEL_ERROR_API : MODEL_ERROR_INCOMPLETE;
			self->error_info.http_status = http_status > 0 ? http_status : 0;
			self->error_info.retryable = 0;
		}
		if (self->error_info.kind == MODEL_ERROR_CONTEXT) {
			if (error_detail)
				*error_detail = nb_charp_steal(&detail);
			llm_stream_move(result, &ctx);
			return MORPH_ERR_CONTEXT_LIMIT;
		}
		if (self->error_info.retryable && attempt < retries) {
			long delay = self->error_info.retry_after_seconds > 0
					     ? self->error_info.retry_after_seconds
					     : llm_retry_delay_seconds(attempt);
			if (detail)
				nb_log_write(self ? self->http.logger : NULL, NB_LOG_WARN,
					     "%s: HTTP %d (%s), retry %d/%d in %lds", operation,
					     status, detail, attempt + 1, retries, delay);
			else
				nb_log_write(self ? self->http.logger : NULL, NB_LOG_WARN,
					     "%s: HTTP %d, retry %d/%d in %lds", operation, status,
					     attempt + 1, retries, delay);
			int wait_rc = http_wait_cancelable(&self->http, (unsigned int)delay * 1000);
			if (wait_rc < 0)
				return wait_rc;
			continue;
		}

		if (error_detail)
			*error_detail = nb_charp_steal(&detail);
		if (status >= 0 && status < 400 &&
		    (nb_buf_status(&ctx.accumulated) || nb_buf_status(&ctx.reasoning)))
			status = nb_buf_status(&ctx.accumulated) ? nb_buf_status(&ctx.accumulated)
								 : nb_buf_status(&ctx.reasoning);
		llm_stream_move(result, &ctx);
		return status;
	}
}

static int llm_chat(struct model *self, const char *system_prompt, const char **messages, int n,
		    const struct model_chat_options *opts, sse_callback cb, void *user_data)
{
	int max_tokens;
	long timeout;

	if (!model_api_key_borrow(self)[0]) {
		nb_log_write(self ? self->http.logger : NULL, NB_LOG_ERROR,
			     "llm_chat: no API key configured");
		if (self) {
			model_clear_last_error(self);
			self->error_info.kind = MODEL_ERROR_CONFIG;
		}
		return nb_err_trace(self ? self->http.logger : NULL, MORPH_ERR_NOT_CONFIGURED,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	}

	nb_log_write(self ? self->http.logger : NULL, NB_LOG_DEBUG,
		     "llm_chat: start, model=%s, api_base=%s", self->model_id, self->api_base);
	max_tokens = opts && opts->max_tokens > 0 ? opts->max_tokens : self->max_tokens;
	timeout = opts && opts->timeout_seconds > 0
			  ? opts->timeout_seconds
			  : (self->timeout_seconds > 0 ? self->timeout_seconds : 300L);

	char *body NB_CLEANUP(cJSON_FreeTextOwner) = NULL;
	size_t body_len = 0;
	int rc = llm_chat_body_create(self ? self->http.logger : NULL, self->model_id,
				      system_prompt, messages, n, max_tokens, 1,
				      self->extra_body_json, llm_adapter_borrow(self),
				      opts ? opts->disable_thinking : 0, &body, &body_len);
	if (rc < 0)
		return rc;

	size_t clean_len = nb_utf8_sanitize_into(body, body, body_len);
	body[clean_len] = '\0';
	body_len = clean_len;

	char url[512];
	snprintf(url, sizeof(url), "%s/chat/completions", self->api_base);

	nb_buf auth_header NB_CLEANUP(nb_buf_cleanup) = {0};
	rc = model_authorization_header_create(self, &auth_header);
	if (rc != 0)
		return rc;
	const char *extra_headers[] = {nb_buf_cstr(&auth_header)};

	nb_log_write(self ? self->http.logger : NULL, NB_LOG_DEBUG,
		     "llm_chat: sending SSE request to %s, timeout=%lds", url, timeout);
	struct llm_stream_ctx ctx NB_CLEANUP(llm_stream_cleanup) = {0};
	char *detail NB_CLEANUP(nb_charp_cleanup) = NULL;
	int status = llm_post_sse_with_retry(self, "llm_chat", opts ? opts->usage_phase : NULL, url,
						     body, body_len, extra_headers, timeout, cb,
						     NULL, NULL, 1,
					     user_data, &ctx, &detail);
	nb_log_write(self ? self->http.logger : NULL, NB_LOG_DEBUG,
		     "llm_chat: SSE request done, status=%d", status);

	if (status < 0) {
		model_set_last_error(
			self, "%s",
			detail ? detail
			       : nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, status));
		nb_log_write(self ? self->http.logger : NULL, NB_LOG_ERROR,
			     "llm_chat: SSE request failed: %s",
			     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, status));
		return status;
	}
	if (status >= 400) {
		if (detail)
			nb_log_write(self ? self->http.logger : NULL, NB_LOG_ERROR,
				     "llm_chat: API returned HTTP %d: %s", status, detail);
		else
			nb_log_write(self ? self->http.logger : NULL, NB_LOG_ERROR,
				     "llm_chat: API returned HTTP %d", status);
		return nb_err_trace(self ? self->http.logger : NULL, MORPH_ERR_API,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
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
		return NB_ERR_NOMEM;
	cJSON_AddStringToObject(msg, "role", "system");
	cJSON_AddStringToObject(msg, "content", system_prompt);
	cJSON_AddItemToArray(messages, msg);
	return 0;
}

static int llm_chat_with_image(struct model *self, const char *system_prompt, const char *prompt,
			       const char *image_path, const struct model_image_chat_options *opts,
			       sse_callback cb, void *user_data)
{
	int max_tokens;
	int max_dim;
	long timeout;

	model_clear_last_error(self);
	if (!model_api_key_borrow(self)[0]) {
		nb_log_write(self ? self->http.logger : NULL, NB_LOG_ERROR,
			     "llm_chat_with_image: no API key configured");
		if (self) {
			model_clear_last_error(self);
			self->error_info.kind = MODEL_ERROR_CONFIG;
		}
		return nb_err_trace(self ? self->http.logger : NULL, MORPH_ERR_NOT_CONFIGURED,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	}
	if (!prompt || !*prompt || !image_path || !*image_path)
		return NB_ERR_INVAL;

	max_tokens = opts && opts->max_tokens > 0
			     ? opts->max_tokens
			     : (self->max_tokens > 0 ? self->max_tokens : 4096);
	max_dim = opts && opts->max_dim > 0 ? opts->max_dim : LLM_IMAGE_QA_MAX_DIM;
	timeout = opts && opts->timeout_seconds > 0
			  ? opts->timeout_seconds
			  : (self->timeout_seconds > 0 ? self->timeout_seconds : 300L);

	char *data_uri NB_CLEANUP(nb_charp_cleanup) = NULL;
	int encode_rc = image_encode_data_uri_copy(image_path, max_dim, &data_uri);
	if (encode_rc < 0) {
		nb_log_write(self ? self->http.logger : NULL, NB_LOG_ERROR,
			     "llm_chat_with_image: failed to encode image: %s", image_path);
		model_set_last_error(self, "failed to read or encode image: %s", image_path);
		return nb_err_trace(self ? self->http.logger : NULL, encode_rc,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	}

	cJSON *root = cJSON_CreateObject();
	cJSON *messages = cJSON_CreateArray();
	if (!root || !messages) {
		cJSON_Delete(root);
		cJSON_Delete(messages);
		return NB_ERR_NOMEM;
	}
	cJSON_AddStringToObject(root, "model", self->model_id);
	cJSON_AddItemToObject(root, "messages", messages);
	if (add_system_message(messages, system_prompt) < 0) {
		cJSON_Delete(root);
		return NB_ERR_NOMEM;
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
		return NB_ERR_NOMEM;
	}
	cJSON_AddStringToObject(user, "role", "user");
	cJSON_AddItemToObject(user, "content", content);
	cJSON_AddStringToObject(text, "type", "text");
	cJSON_AddStringToObject(text, "text", prompt);
	cJSON_AddItemToArray(content, text);

	cJSON_AddStringToObject(image, "type", "image_url");
	cJSON *uri_value = cJSON_CreateString(data_uri);
	if (uri_value)
		nb_charp_cleanup(&data_uri);
	if (!uri_value || !cJSON_AddItemToObject(image_url, "url", uri_value)) {
		cJSON_Delete(uri_value);
		cJSON_Delete(image_url);
		cJSON_Delete(image);
		cJSON_Delete(user);
		cJSON_Delete(root);
		return nb_err_trace(self ? self->http.logger : NULL, NB_ERR_NOMEM,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
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

	char *body NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!body)
		return NB_ERR_NOMEM;

	size_t body_len = strlen(body);
	size_t clean_len = nb_utf8_sanitize_into(body, body, body_len);
	body[clean_len] = '\0';
	body_len = clean_len;

	char url[512];
	snprintf(url, sizeof(url), "%s/chat/completions", self->api_base);

	nb_buf auth_header NB_CLEANUP(nb_buf_cleanup) = {0};
	int auth_rc = model_authorization_header_create(self, &auth_header);
	if (auth_rc != 0)
		return auth_rc;
	const char *extra_headers[] = {nb_buf_cstr(&auth_header)};

	nb_log_write(self ? self->http.logger : NULL, NB_LOG_DEBUG,
		     "llm_chat_with_image: sending SSE request to %s, timeout=%lds, max_tokens=%d, "
		     "max_dim=%d, body=%zu",
		     url, timeout, max_tokens, max_dim, body_len);
	struct llm_stream_ctx ctx NB_CLEANUP(llm_stream_cleanup) = {0};
	char *detail NB_CLEANUP(nb_charp_cleanup) = NULL;
	int status = llm_post_sse_with_retry(self, "llm_chat_with_image", NULL, url, body, body_len,
					     extra_headers, timeout, cb, NULL, NULL, 1, user_data,
					     &ctx, &detail);
	nb_log_write(self ? self->http.logger : NULL, NB_LOG_DEBUG,
		     "llm_chat_with_image: SSE request done, status=%d", status);

	if (status < 0) {
		nb_log_write(self ? self->http.logger : NULL, NB_LOG_ERROR,
			     "llm_chat_with_image: SSE request failed: %d", status);
		model_set_last_error(
			self, "HTTP/SSE request failed: %s",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, status));
		return status;
	}
	if (status >= 400) {
		if (detail) {
			nb_log_write(self ? self->http.logger : NULL, NB_LOG_ERROR,
				     "llm_chat_with_image: API returned HTTP %d: %s", status,
				     detail);
			model_set_last_error(self, "HTTP %d: %s", status, detail);
		} else {
			nb_log_write(self ? self->http.logger : NULL, NB_LOG_ERROR,
				     "llm_chat_with_image: API returned HTTP %d", status);
			model_set_last_error(self, "HTTP %d from %s", status, self->api_base);
		}
		return nb_err_trace(self ? self->http.logger : NULL, MORPH_ERR_API,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
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

int model_input_snapshot_create(const struct model *model, const char *system_prompt,
				const struct chat_message *messages, int msg_count,
				const struct tool_desc *tools, int tool_count, cJSON **out)
{
	const struct llm_codec *codec = llm_codec_borrow(model);
	cJSON *root;
	cJSON *input;
	cJSON *definitions;

	if (!out || msg_count < 0 || tool_count < 0 || (msg_count && !messages) ||
	    (tool_count && !tools))
		return nb_err_trace(model ? model->http.logger : NULL, NB_ERR_INVAL,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	if (*out)
		return NB_ERR_EXISTS;
	root = cJSON_CreateObject();
	input = codec->input(model, system_prompt, messages, msg_count);
	definitions = codec->tools(tools, tool_count);
	if (!root || !input || !definitions) {
		cJSON_Delete(root);
		cJSON_Delete(input);
		cJSON_Delete(definitions);
		return nb_err_trace(model ? model->http.logger : NULL, NB_ERR_NOMEM,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	}
	if (!cJSON_AddItemToObject(root, codec->input_key, input)) {
		cJSON_Delete(input);
		cJSON_Delete(definitions);
		cJSON_Delete(root);
		return nb_err_trace(model ? model->http.logger : NULL, NB_ERR_NOMEM,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	}
	if (!cJSON_AddItemToObject(root, "tools", definitions)) {
		cJSON_Delete(definitions);
		cJSON_Delete(root);
		return nb_err_trace(model ? model->http.logger : NULL, NB_ERR_NOMEM,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	}
	if (codec->system_key && system_prompt && *system_prompt &&
	    !cJSON_AddStringToObject(root, codec->system_key, system_prompt)) {
		cJSON_Delete(root);
		return nb_err_trace(model ? model->http.logger : NULL, NB_ERR_NOMEM,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	}
	*out = root;
	return 0;
}

int model_estimate_input_tokens(const struct model *model, const char *system_prompt,
				const struct chat_message *messages, int msg_count,
				const struct tool_desc *tools, int tool_count,
				model_token_counter count, void *user_data, int *tokens)
{
	const struct llm_codec *codec = llm_codec_borrow(model);
	cJSON *input;
	cJSON *wire_tools;
	char *json;
	int64_t total;

	if (!count || !tokens || msg_count < 0 || tool_count < 0 || (msg_count && !messages) ||
	    (tool_count && !tools))
		return nb_err_trace(model ? model->http.logger : NULL, NB_ERR_INVAL,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	input = codec->input(model, system_prompt, messages, msg_count);
	if (!input)
		return nb_err_trace(model ? model->http.logger : NULL, NB_ERR_NOMEM,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	total = 16 + input_text_tokens(input, count, user_data);
	cJSON_Delete(input);
	if (codec->system_key && system_prompt && system_prompt[0])
		total += count(system_prompt, user_data);
	if (tool_count > 0) {
		wire_tools = codec->tools(tools, tool_count);
		if (!wire_tools)
			return nb_err_trace(model ? model->http.logger : NULL, NB_ERR_NOMEM,
					    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
					    __LINE__);
		/* Schema keys and constraints are part of the model-visible definition. */
		json = cJSON_PrintUnformatted(wire_tools);
		cJSON_Delete(wire_tools);
		if (!json)
			return nb_err_trace(model ? model->http.logger : NULL, NB_ERR_NOMEM,
					    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
					    __LINE__);
		total += count(json, user_data);
		cJSON_free(json);
	}
	*tokens = total > INT_MAX ? INT_MAX : (int)total;
	return 0;
}

static int llm_chat_with_tools_impl(struct model *self, const char *system_prompt,
				    struct chat_message *messages, int msg_count,
				    struct tool_desc *tools, int tool_count,
				    struct chat_response *response, sse_callback thought_cb,
				    llm_stream_callback stream_cb, void *stream_ud)
{
	const struct llm_codec *codec = llm_codec_borrow(self);
	const struct llm_adapter *adapter = llm_adapter_borrow(self);
	struct llm_stream_ctx ctx NB_CLEANUP(llm_stream_cleanup) = {0};
	cJSON *root = NULL;
	char *body NB_CLEANUP(cJSON_FreeTextOwner) = NULL;
	char url[512];
	nb_buf authorization NB_CLEANUP(nb_buf_cleanup) = {0};
	char *detail NB_CLEANUP(nb_charp_cleanup) = NULL;
	long timeout;
	int rc;
	int status;

	if (!self || !response)
		return NB_ERR_INVAL;
	if (response->content || response->reasoning_content || response->tool_calls ||
	    response->tool_call_count)
		return NB_ERR_EXISTS;
	model_clear_last_error(self);
	if (!model_api_key_borrow(self)[0]) {
		self->error_info.kind = MODEL_ERROR_CONFIG;
		model_set_last_error(self, "no API key configured");
		return MORPH_ERR_NOT_CONFIGURED;
	}
	memset(response, 0, sizeof(*response));
	rc = model_input_snapshot_create(self, system_prompt, messages, msg_count, tools,
					 tool_count, &root);
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
	body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!body)
		return NB_ERR_NOMEM;
	body[nb_utf8_sanitize_into(body, body, strlen(body))] = '\0';
	snprintf(url, sizeof(url), "%s%s", self->api_base, codec->endpoint);
	rc = model_authorization_header_create(self, &authorization);
	if (rc != 0)
		return rc;
	const char *headers[] = {nb_buf_cstr(&authorization)};
	timeout = self->timeout_seconds > 0 ? self->timeout_seconds : 300L;
	status = llm_post_sse_with_retry(self, "llm_chat_with_tools", NULL, url, body, strlen(body),
					 headers, timeout, thought_cb, stream_cb, codec->event,
					 codec->requires_terminal_event, stream_ud, &ctx, &detail);
	llm_usage_finalize(self, &ctx, strlen(body));
	if (ctx.accumulated.len > 0) {
		response->content = strdup(nb_buf_cstr(&ctx.accumulated));
		if (!response->content)
			return NB_ERR_NOMEM;
	}
	if (adapter->capture_reasoning && ctx.reasoning.len > 0) {
		response->reasoning_content = strdup(nb_buf_cstr(&ctx.reasoning));
		if (!response->reasoning_content)
			return NB_ERR_NOMEM;
	}
	int call_rc = llm_stream_tool_calls_move(&ctx, response);
	if (call_rc)
		return call_rc;
	response->usage = ctx.usage;
	if ((status >= 0 && status < 400) || !strcmp(response->usage.usage_source, "provider"))
		model_report_usage(self, &response->usage);
	if (status < 0) {
		model_set_last_error(
			self, "%s",
			detail ? detail
			       : nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, status));
		return status;
	}
	if (status >= 400) {
		model_set_last_error(self, "HTTP %d: %s", status,
				     detail ? detail : "model request failed");
		if (!response->content)
			response->content = nb_charp_steal(&detail);
		return MORPH_ERR_API;
	}
	return status;
}

static int llm_chat_with_tools(struct model *self, const char *system_prompt,
			       struct chat_message *messages, int msg_count,
			       struct tool_desc *tools, int tool_count,
			       struct chat_response *response, sse_callback thought_cb,
			       void *thought_ud)
{
	return llm_chat_with_tools_impl(self, system_prompt, messages, msg_count, tools, tool_count,
					response, thought_cb, NULL, thought_ud);
}

static int llm_chat_with_tools_stream(struct model *self, const char *system_prompt,
				      struct chat_message *messages, int msg_count,
				      struct tool_desc *tools, int tool_count,
				      struct chat_response *response, llm_stream_callback stream_cb,
				      void *stream_ud)
{
	return llm_chat_with_tools_impl(self, system_prompt, messages, msg_count, tools, tool_count,
					response, NULL, stream_cb, stream_ud);
}

static int llm_generate(struct model *self, const char *prompt, const char *out_path)
{
	if (!self || !prompt)
		return NB_ERR_INVAL;

	const char *messages[] = {prompt};
	char *body NB_CLEANUP(cJSON_FreeTextOwner) = NULL;
	size_t body_len = 0;
	int rc = llm_chat_body_create(self ? self->http.logger : NULL, self->model_id, NULL,
				      messages, 1, self->max_tokens, 0, self->extra_body_json, NULL,
				      0, &body, &body_len);
	if (rc < 0) {
		return rc;
	}

	struct http_response resp = {0};
	char url[512];
	snprintf(url, sizeof(url), "%s/chat/completions", self->api_base);
	nb_buf auth_header NB_CLEANUP(nb_buf_cleanup) = {0};
	rc = model_authorization_header_create(self, &auth_header);
	if (rc != 0)
		return rc;
	const char *extra_headers[] = {nb_buf_cstr(&auth_header)};
	rc = http_post_ex(&self->http, url, body, body_len, "application/json", extra_headers, 1,
			  &resp);

	if (rc < 0) {
		http_response_free(&resp);
		return rc;
	}

	if (resp.status_code != 200) {
		nb_log_write(self ? self->http.logger : NULL, NB_LOG_ERROR,
			     "llm_generate: API returned %d", resp.status_code);
		http_response_free(&resp);
		return nb_err_trace(self ? self->http.logger : NULL, MORPH_ERR_API,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	}

	rc = 0;
	if (out_path && resp.body.ptr)
		rc = nb_file_write_all(out_path, resp.body.ptr, resp.body.len, NULL);

	http_response_free(&resp);
	return rc;
}

static void llm_destroy(struct model *self)
{
	if (self) {
		free(self->api_key_owned);
		free(self);
	}
}

static void llm_owner_cleanup(struct model **owner)
{
	if (!owner)
		return;
	llm_destroy(*owner);
	*owner = NULL;
}

static struct model *llm_owner_steal(struct model **owner)
{
	struct model *model = *owner;

	*owner = NULL;
	return model;
}

struct model *model_llm_create(struct nb_log *logger, const char *provider, const char *model_id,
			       const char *api_base, const char *api_key)
{
	struct model *m NB_CLEANUP(llm_owner_cleanup) = calloc(1, sizeof(*m));
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
	if (api_key && strlen(api_key) >= sizeof(m->api_key)) {
		m->api_key_owned = strdup(api_key);
		if (!m->api_key_owned)
			return NULL;
	} else if (api_key) {
		memcpy(m->api_key, api_key, strlen(api_key) + 1);
	}
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
	return llm_owner_steal(&m);
}

struct model *model_llm_clone(const struct model *source, struct nb_log *logger)
{
	struct model *copy;

	if (!source)
		return NULL;
	copy = model_llm_create(logger, source->provider, source->model_id, source->api_base,
				model_api_key_borrow(source));
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

int model_llm_set_reasoning_effort(struct model *model, const char *effort)
{
	if (!model || !effort || !*effort)
		return NB_ERR_INVAL;
	cJSON *extra NB_CLEANUP(cJSON_DeleteOwner) = model->extra_body_json[0]
							     ? cJSON_Parse(model->extra_body_json)
							     : cJSON_CreateObject();
	if (!cJSON_IsObject(extra))
		return MORPH_ERR_CONFIG;
	cJSON_DeleteItemFromObjectCaseSensitive(extra, "reasoning_effort");
	const struct llm_adapter *adapter = llm_adapter_borrow(model);
	if (adapter->responses_api) {
		cJSON *reasoning = cJSON_GetObjectItemCaseSensitive(extra, "reasoning");
		if (!reasoning)
			reasoning = cJSON_AddObjectToObject(extra, "reasoning");
		if (!cJSON_IsObject(reasoning))
			return MORPH_ERR_CONFIG;
		cJSON_DeleteItemFromObjectCaseSensitive(reasoning, "effort");
		if (!cJSON_AddStringToObject(reasoning, "effort", effort))
			return NB_ERR_NOMEM;
	} else if (!cJSON_AddStringToObject(extra, "reasoning_effort", effort)) {
		return NB_ERR_NOMEM;
	}
	char *json NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(extra);
	if (!json)
		return NB_ERR_NOMEM;
	if (strlen(json) >= sizeof(model->extra_body_json))
		return NB_ERR_TOO_BIG;
	strcpy(model->extra_body_json, json);
	return 0;
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
	free(resp->content);
	resp->content = NULL;
	free(resp->reasoning_content);
	resp->reasoning_content = NULL;
	for (int i = 0; i < resp->tool_call_count; i++)
		tool_call_cleanup(&resp->tool_calls[i]);
	free(resp->tool_calls);
	resp->tool_calls = NULL;
	resp->tool_call_count = 0;
}

void chat_message_cleanup(struct chat_message *msg)
{
	if (!msg)
		return;
	free(msg->role);
	free(msg->content);
	free(msg->tool_call_id);
	free(msg->reasoning_content);
	for (int i = 0; i < msg->tool_call_count; i++)
		tool_call_cleanup(&msg->tool_calls[i]);
	free(msg->tool_calls);
	memset(msg, 0, sizeof(*msg));
}

void tool_call_cleanup(struct tool_call *tc)
{
	if (!tc)
		return;
	free(tc->id);
	free(tc->tool_call_id);
	free(tc->name);
	free(tc->provider_item_id);
	free(tc->arguments);
	memset(tc, 0, sizeof(*tc));
}

int tool_call_clone(const struct tool_call *source, struct tool_call *out)
{
	if (!source || !out || source == out)
		return NB_ERR_INVAL;
	if (out->id || out->tool_call_id || out->name || out->provider_item_id || out->arguments)
		return NB_ERR_EXISTS;
	struct tool_call copy NB_CLEANUP(tool_call_cleanup) = {0};
	const char *values[] = {source->id, source->tool_call_id, source->name,
				 source->provider_item_id, source->arguments};
	char **slots[] = {&copy.id, &copy.tool_call_id, &copy.name,
			  &copy.provider_item_id, &copy.arguments};

	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		if (values[i]) {
			*slots[i] = strdup(values[i]);
			if (!*slots[i])
				return NB_ERR_NOMEM;
		}
	}
	copy.input_kind = source->input_kind;
	*out = copy;
	memset(&copy, 0, sizeof(copy));
	return 0;
}

int chat_message_clone(const struct chat_message *source, struct chat_message *out)
{
	if (!source || !out || source == out || source->tool_call_count < 0 ||
	    (source->tool_call_count && !source->tool_calls))
		return NB_ERR_INVAL;
	if (out->role || out->content || out->tool_call_id || out->tool_calls ||
	    out->reasoning_content || out->tool_call_count)
		return NB_ERR_EXISTS;
	struct chat_message copy NB_CLEANUP(chat_message_cleanup) = {0};
	const char *values[] = {source->role, source->content, source->tool_call_id,
				source->reasoning_content};
	char **slots[] = {&copy.role, &copy.content, &copy.tool_call_id, &copy.reasoning_content};
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		if (values[i]) {
			*slots[i] = strdup(values[i]);
			if (!*slots[i])
				return NB_ERR_NOMEM;
		}
	}
	if ((size_t)source->tool_call_count > SIZE_MAX / sizeof(*copy.tool_calls))
		return NB_ERR_RANGE;
	if (source->tool_call_count) {
		copy.tool_calls = calloc((size_t)source->tool_call_count, sizeof(*copy.tool_calls));
		if (!copy.tool_calls)
			return NB_ERR_NOMEM;
		copy.tool_call_count = source->tool_call_count;
		for (int i = 0; i < copy.tool_call_count; i++) {
			int rc = tool_call_clone(&source->tool_calls[i], &copy.tool_calls[i]);
			if (rc != 0)
				return rc;
		}
	}
	copy.is_context_summary = source->is_context_summary;
	*out = copy;
	memset(&copy, 0, sizeof(copy));
	return 0;
}
