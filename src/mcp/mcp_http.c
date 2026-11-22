#include "mcp/mcp_internal.h"
#include "cJSON.h"
#include <nb/sse.h>
#include <nb/log.h>
#include <nb/buf.h>
#include <nb/encoding.h>
#include <nb/cleanup.h>
#include <nb/time.h>
#include "errors.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MCP_AUTH_TOKEN_PLACEHOLDER "${auth_token}"

struct mcp_sse_extract {
	nb_buf data;
	nb_buf raw;
	nb_sse *parser;
	struct mcp_client *client;
	char *session_id;
	char *last_event_id;
	int seen_data;
	int expected_id;
	int streaming;
	int stop_on_response;
	int timed_out;
	int timeout_ms;
	int64_t started_ms;
	int64_t waited_ms;
};

static int mcp_http_send_peer_reply(struct mcp_client *client, const char *session_id,
				    const char *json);

static void mcp_sse_extract_cleanup(struct mcp_sse_extract *ctx)
{
	nb_buf_cleanup(&ctx->data);
	nb_buf_cleanup(&ctx->raw);
	nb_sse_free(ctx->parser);
	free(ctx->session_id);
	free(ctx->last_event_id);
}

static nb_status mcp_sse_extract_cb(nb_sse *parser, const nb_sse_event *event, void *user_data,
				    nb_err *err)
{
	struct mcp_sse_extract *ctx = user_data;
	int rc;

	(void)parser;
	(void)err;
	nb_str data = event->value.message.data;
	if (!ctx)
		return NB_OK;

	nb_str event_id = event->value.message.id;
	if (event_id.len) {
		char *copy NB_CLEANUP(nb_charp_cleanup) = strndup(event_id.ptr, event_id.len);
		if (!copy)
			return NB_ERR_NOMEM;

		free(ctx->last_event_id);
		ctx->last_event_id = nb_charp_steal(&copy);
	}

	if (!data.len)
		return NB_OK;
	if (ctx->expected_id >= 0 || ctx->expected_id == -2) {
		cJSON *object = cJSON_ParseWithLength(data.ptr, data.len);
		cJSON *id = cJSON_GetObjectItemCaseSensitive(object, "id");
		cJSON *error = cJSON_GetObjectItemCaseSensitive(object, "error");
		cJSON *method = cJSON_GetObjectItemCaseSensitive(object, "method");
		if (method && ctx->client) {
			char *reply NB_CLEANUP(nb_charp_cleanup) = NULL;
			rc = ctx->client->protocol && ctx->client->protocol->per_request && id
				     ? NB_ERR_PROTOCOL
				     : mcp_dispatch_message(ctx->client, object, &reply);
			cJSON_Delete(object);
			if (rc == 0 && reply)
				rc = mcp_http_send_peer_reply(ctx->client, ctx->session_id, reply);

			return rc;
		}

		int matches = (cJSON_IsNumber(id) && id->valuedouble == ctx->expected_id) ||
			      (!id && error);

		cJSON_Delete(object);
		if (!matches)
			return 0;
	}
	nb_buf_clear(&ctx->data);
	rc = (nb_buf_append_str(&ctx->data, data) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	ctx->seen_data = 1;
	return ctx->stop_on_response ? NB_ERR_CANCELLED : 0;
}

static nb_status mcp_http_stream_headers(nb_http_call *call, const nb_http_response *response,
					 void *opaque, nb_err *error)
{
	(void)call;
	(void)error;
	struct mcp_sse_extract *ctx = opaque;
	nb_str value;
	if (nb_http_headers_get(response->headers, NB_STR_LIT("Content-Type"), &value))
		ctx->streaming =
			value.len >= 17 && !strncasecmp(value.ptr, "text/event-stream", 17);

	if (nb_http_headers_get(response->headers, NB_STR_LIT("Mcp-Session-Id"), &value)) {
		free(ctx->session_id);
		ctx->session_id = strndup(value.ptr, value.len);
		if (!ctx->session_id)
			return NB_ERR_NOMEM;
	}

	return 0;
}

static nb_status mcp_http_stream_data(nb_http_call *call, nb_str chunk, void *opaque, nb_err *error)
{
	(void)call;
	struct mcp_sse_extract *ctx = opaque;
	if (ctx->streaming)
		return nb_sse_feed(ctx->parser, chunk.ptr, chunk.len, error);

	return nb_buf_append_str(&ctx->raw, chunk) ? 0 : NB_ERR_NOMEM;
}

static int mcp_http_stream_interrupted(void *opaque)
{
	struct mcp_sse_extract *ctx = opaque;
	struct mcp_client *client = ctx->client;
	if (client->peer_cancelled)
		return 1;

	if (client->notification_target &&
	    nb_atomic_int_load(&client->notification_target->subscription_ended))
		return 1;

	if (mcp_client_interrupted(client))
		return 1;

	ctx->timed_out = ctx->timeout_ms > 0 &&
			 nb_time_monotonic_ms() - ctx->started_ms -
					 (client->interaction_wait_ms - ctx->waited_ms) >=
				 ctx->timeout_ms;
	return ctx->timed_out;
}

static char *mcp_http_sse_response(const char *raw, size_t len, int expected_id)
{
	struct mcp_sse_extract ctx NB_CLEANUP(mcp_sse_extract_cleanup) = {0};
	nb_sse *parser = NULL;
	char *json = NULL;
	int rc;

	if (!raw || len == 0)
		return NULL;

	rc = (nb_buf_init(&ctx.data), nb_buf_reserve(&ctx.data, 1024) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return NULL;
	ctx.seen_data = 0;
	ctx.expected_id = expected_id;

	nb_sse_options options = {0};
	rc = nb_sse_new(&options, &parser, NULL);
	if (rc == NB_OK)
		rc = nb_sse_on(parser, NB_SSE_MESSAGE, mcp_sse_extract_cb, &ctx, NULL, NULL);

	if (rc == NB_OK)
		rc = nb_sse_feed(parser, raw, len, NULL);

	/* Preserve MCP's tolerance of a missing final event separator. */
	if (rc == NB_OK)
		rc = nb_sse_feed(parser, "\n", 1, NULL);

	if (rc == NB_OK)
		rc = nb_sse_finish(parser, NULL);

	nb_sse_free(parser);

	if (rc == 0 && ctx.seen_data)
		json = (nb_buf_ok(&ctx.data) ? strdup(nb_buf_cstr(&ctx.data)) : NULL);
	else
		nb_buf_cleanup(&ctx.data);
	return json;
}

char *mcp_http_sse_json_create(const char *raw, size_t len)
{
	return mcp_http_sse_response(raw, len, -1);
}

int mcp_http_url_uses_auth_token(const char *url)
{
	return url && strstr(url, MCP_AUTH_TOKEN_PLACEHOLDER) != NULL;
}

int mcp_http_request_url_create(const struct mcp_server_config *cfg, char **out_url)
{
	const char *url;
	const char *token;
	const char *p;
	const char *match;
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	size_t placeholder_len = strlen(MCP_AUTH_TOKEN_PLACEHOLDER);
	int rc;

	if (!cfg || !out_url)
		return NB_ERR_INVAL;
	if (*out_url)
		return NB_ERR_EXISTS;

	url = cfg->http_url;
	if (!url || !url[0])
		return NB_ERR_INVAL;

	if (!mcp_http_url_uses_auth_token(url)) {
		*out_url = strdup(url);
		if (!*out_url)
			return NB_ERR_NOMEM;
		return 0;
	}

	if (!cfg->http_auth_token_env[0]) {
		nb_log_write(cfg ? cfg->logger : NULL, NB_LOG_ERROR,
			     "mcp http: URL uses %s but auth_token_env is not set",
			     MCP_AUTH_TOKEN_PLACEHOLDER);
		return MORPH_ERR_NOT_CONFIGURED;
	}

	token = getenv(cfg->http_auth_token_env);
	if (!token || !token[0]) {
		nb_log_write(cfg ? cfg->logger : NULL, NB_LOG_ERROR,
			     "mcp http: URL uses %s but env var '%s' is not set",
			     MCP_AUTH_TOKEN_PLACEHOLDER, cfg->http_auth_token_env);
		return MORPH_ERR_NOT_CONFIGURED;
	}

	rc = (nb_buf_init(&buf),
	      nb_buf_reserve(&buf, strlen(url) + strlen(token)) ? 0 : NB_ERR_NOMEM);
	if (rc < 0)
		return rc;

	p = url;
	while ((match = strstr(p, MCP_AUTH_TOKEN_PLACEHOLDER)) != NULL) {
		rc = (nb_buf_append(&buf, p, (size_t)(match - p)) ? 0 : NB_ERR_NOMEM);
		if (rc < 0)
			return rc;
		rc = nb_url_encode(nb_str_from_cstr(token), false, &buf, NULL);
		if (rc < 0)
			return rc;
		p = match + placeholder_len;
	}
	rc = (nb_buf_append_cstr(&buf, p) ? 0 : NB_ERR_NOMEM);
	if (rc < 0)
		return rc;

	*out_url = (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
	return *out_url ? 0 : NB_ERR_NOMEM;
}

static int mcp_http_redact_query_value(nb_buf *buf, const char *start, const char *end)
{
	const char *eq = memchr(start, '=', (size_t)(end - start));
	int rc;

	if (!eq)
		return (nb_buf_append(buf, start, (size_t)(end - start)) ? 0 : NB_ERR_NOMEM);

	rc = (nb_buf_append(buf, start, (size_t)(eq - start + 1)) ? 0 : NB_ERR_NOMEM);
	if (rc < 0)
		return rc;
	if (eq + 1 < end)
		return (nb_buf_append_cstr(buf, "***") ? 0 : NB_ERR_NOMEM);
	return 0;
}

static char *mcp_http_redact_url_for_log(const char *url)
{
	const char *q;
	const char *p;
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc;

	if (!url)
		return NULL;

	q = strchr(url, '?');
	if (!q)
		return strdup(url);

	rc = (nb_buf_init(&buf), nb_buf_reserve(&buf, strlen(url)) ? 0 : NB_ERR_NOMEM);
	if (rc < 0)
		return NULL;

	rc = (nb_buf_append(&buf, url, (size_t)(q - url + 1)) ? 0 : NB_ERR_NOMEM);
	if (rc < 0)
		return NULL;

	p = q + 1;
	while (*p && *p != '#') {
		const char *end = p;

		while (*end && *end != '&' && *end != '#')
			end++;
		rc = mcp_http_redact_query_value(&buf, p, end);
		if (rc < 0)
			return NULL;
		if (*end == '&') {
			rc = (nb_buf_append_char(&buf, '&') ? 0 : NB_ERR_NOMEM);
			if (rc < 0)
				return NULL;
			p = end + 1;
		} else {
			p = end;
		}
	}
	if (*p == '#') {
		rc = (nb_buf_append_cstr(&buf, p) ? 0 : NB_ERR_NOMEM);
		if (rc < 0)
			return NULL;
	}

	return (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
}

static int mcp_http_resume(struct mcp_client *client, const struct mcp_wire_request *original,
			   const struct mcp_sse_extract *stream, int timeout_ms,
			   struct mcp_wire_reply *reply)
{
	uint64_t retry = 0;
	(void)nb_sse_retry_ms(stream->parser, &retry);
	if (timeout_ms > 0 && retry >= (uint64_t)timeout_ms)
		return NB_ERR_TIMEOUT;

	int64_t started = nb_time_monotonic_ms();
	struct http_control control = {.interrupt_check = mcp_client_interrupted,
				       .interrupt_user_data = client};
	while (retry) {
		unsigned int wait = retry > 1000 ? 1000 : (unsigned int)retry;
		int rc = http_wait_cancelable(&control, wait);
		if (rc != 0)
			return rc;

		retry -= wait;
	}

	if (timeout_ms > 0) {
		int64_t remaining = timeout_ms - (nb_time_monotonic_ms() - started);
		if (remaining <= 0)
			return NB_ERR_TIMEOUT;

		timeout_ms = (int)remaining;
	}

	struct mcp_wire_request request NB_CLEANUP(mcp_wire_request_cleanup) = {
		.id = original->id,
		.http_method = "GET",
		.resume_count = original->resume_count + 1,
	};
	const char *header = NULL;
	nb_vec_init(&request.headers, sizeof(header));
	for (size_t i = 0; i < original->headers.len; i++) {
		header = *(const char **)nb_vec_at(&original->headers, i);
		if (!strncmp(header, "Last-Event-ID:", 14))
			continue;

		char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(header);
		if (!copy)
			return NB_ERR_NOMEM;

		if (!nb_vec_push_copy(&request.headers, &copy))
			return NB_ERR_NOMEM;

		copy = NULL;
	}

	int rc = mcp_protocol_add_header_copy(&request.headers, "Last-Event-ID",
					      stream->last_event_id);
	if (rc == 0)
		rc = mcp_http_exchange(client, &request, timeout_ms, reply);

	return rc;
}

int mcp_http_exchange(struct mcp_client *client, const struct mcp_wire_request *request,
		      int timeout_ms, struct mcp_wire_reply *reply)
{
	nb_vec headers NB_CLEANUP(nb_vec_cleanup) = {0};
	struct mcp_wire_reply received NB_CLEANUP(mcp_wire_reply_cleanup) = {0};
	struct mcp_sse_extract stream NB_CLEANUP(mcp_sse_extract_cleanup) = {0};
	nb_buf authorization NB_CLEANUP(nb_buf_cleanup) = {0};
	char *url NB_CLEANUP(nb_charp_cleanup) = NULL;
	int rc;

	if (!client || !request || (!request->json && !request->http_method) || !reply ||
	    !client->http_client)
		return NB_ERR_INVAL;
	if (reply->json || reply->session_id)
		return NB_ERR_EXISTS;
	if (request->headers.len > INT_MAX - 2)
		return NB_ERR_RANGE;
	rc = (nb_vec_init(&headers, sizeof(url)),
	      nb_vec_reserve(&headers, request->headers.len + 2) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	const char **values = nb_vec_push_n(&headers, request->headers.len + 2);

	if (!values) {
		return NB_ERR_NOMEM;
	}
	values[0] = "Accept: application/json, text/event-stream";
	for (size_t i = 0; i < request->headers.len; i++)
		values[i + 1] = *(const char **)nb_vec_at(&request->headers, i);
	headers.len = request->headers.len + 1;
	if (client->config.http_auth_token_env[0] &&
	    !mcp_http_url_uses_auth_token(client->config.http_url)) {
		const char *token = getenv(client->config.http_auth_token_env);

		if (!token || !token[0]) {
			rc = (MORPH_ERR_NOT_CONFIGURED);
			return rc;
		}
		rc = (nb_buf_init(&authorization),
		      nb_buf_reserve(&authorization, 128) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_printf(&authorization, "Authorization: Bearer %s", token)
				      ? 0
				      : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
		values[headers.len++] = nb_buf_cstr(&authorization);
	}
	if (headers.len > INT_MAX) {
		rc = (NB_ERR_RANGE);
		return rc;
	}
	rc = mcp_http_request_url_create(&client->config, &url);
	if (rc != 0)
		return rc;
	stream.client = client;
	stream.expected_id = request->id;
	stream.stop_on_response = 1;
	stream.timeout_ms = timeout_ms;
	stream.started_ms = nb_time_monotonic_ms();
	stream.waited_ms = client->interaction_wait_ms;
	nb_buf_init(&stream.data);
	nb_buf_init(&stream.raw);
	nb_sse_options sse_options = {0};
	rc = nb_sse_new(&sse_options, &stream.parser, NULL);
	if (rc == 0)
		rc = nb_sse_on(stream.parser, NB_SSE_MESSAGE, mcp_sse_extract_cb, &stream, NULL,
			       NULL);

	if (rc != 0)
		return rc;

	struct http_control control = {
		.logger = client->config.logger,
		.interrupt_check = mcp_http_stream_interrupted,
		.interrupt_user_data = &stream,
	};
	nb_vec native_headers NB_CLEANUP(nb_vec_cleanup) = {0};
	nb_http_call *call NB_CLEANUP(http_call_cleanup) = NULL;
	nb_http_request http_request;

	nb_vec_init(&native_headers, sizeof(nb_http_header));
	rc = http_headers_parse(&native_headers, (const char *const *)headers.data, headers.len);
	nb_http_header content_type = {NB_STR_LIT("Content-Type"), NB_STR_LIT("application/json")};
	if (rc == NB_OK && !nb_vec_push_copy(&native_headers, &content_type))
		rc = NB_ERR_NOMEM;

	http_request_init(&http_request, request->http_method ? request->http_method : "POST", url);
	http_request.body = nb_str_from_cstr(request->json);
	http_request.headers =
		(nb_http_headers){(const nb_http_header *)native_headers.data, native_headers.len};
	http_request.timeouts.total_ms = 0;
	http_request.timeouts.idle_ms = 0;
	nb_http_handler handler = {.ctx = &stream,
				   .on_headers = mcp_http_stream_headers,
				   .on_data = mcp_http_stream_data};
	if (rc == NB_OK)
		rc = http_request_run(&control, client->http_client, &http_request, &handler,
				      &call);

	const nb_http_result *result = nb_http_call_result(call);
	client->http_status = result && result->response ? result->response->status_code : 0;
	if (stream.seen_data && rc == NB_ERR_CANCELLED)
		rc = 0;

	if (client->peer_cancelled)
		rc = NB_ERR_CANCELLED;

	if (stream.timed_out)
		rc = NB_ERR_TIMEOUT;

	if (client->protocol && !client->protocol->per_request && stream.streaming &&
	    !stream.seen_data && stream.last_event_id && (request->id >= 0 || request->id == -2) &&
	    request->resume_count < 3 &&
	    (rc == 0 || rc == NB_ERR_IO || rc == NB_ERR_CLOSED || rc == NB_ERR_BROKEN_PIPE)) {
		int64_t remaining = timeout_ms - (nb_time_monotonic_ms() - stream.started_ms -
						  (client->interaction_wait_ms - stream.waited_ms));
		if (timeout_ms > 0 && remaining <= 0)
			return NB_ERR_TIMEOUT;

		return mcp_http_resume(client, request, &stream,
				       timeout_ms > 0 ? (int)remaining : 0, reply);
	}

	if (rc != NB_OK)
		return rc;

	received.http_status = client->http_status;
	received.session_id = nb_charp_steal(&stream.session_id);
	if (stream.streaming && !stream.seen_data) {
		rc = nb_sse_feed(stream.parser, "\n", 1, NULL);
		if (rc == 0)
			rc = nb_sse_finish(stream.parser, NULL);

		if (stream.seen_data && rc == NB_ERR_CANCELLED)
			rc = 0;
	}

	if (rc == NB_OK && (stream.seen_data || stream.raw.len)) {
		received.json = strdup(stream.seen_data ? nb_buf_cstr(&stream.data)
							: nb_buf_cstr(&stream.raw));

		if (!received.json)
			rc = NB_ERR_NOMEM;
	}

	if (rc == 0) {
		if (client->http_status == 404 && client->session_id)
			client->connected = 0;

		*reply = received;
		memset(&received, 0, sizeof(received));
	}
	return rc;
}

static int mcp_http_send_peer_reply(struct mcp_client *client, const char *session_id,
				    const char *json)
{
	struct mcp_client peer = {.config = client->config};
	nb_http_client *backend NB_CLEANUP(http_client_cleanup) = NULL;
	int rc = http_client_open(NULL, &backend);
	if (rc != 0)
		return rc;

	peer.http_client = backend;
	peer.interrupt_check = client->interrupt_check;
	peer.interrupt_user_data = client->interrupt_user_data;
	peer.shutdown_cancel = client->shutdown_cancel;
	peer.catalog_cancelled = client->catalog_cancelled;
	peer.refreshing_catalog = nb_atomic_int_load(&client->refreshing_catalog);
	struct mcp_wire_request request NB_CLEANUP(mcp_wire_request_cleanup) = {.id = -1};
	struct mcp_wire_reply reply NB_CLEANUP(mcp_wire_reply_cleanup) = {0};
	request.json = strdup(json);
	nb_vec_init(&request.headers, sizeof(json));
	if (!request.json)
		return NB_ERR_NOMEM;

	if (client->protocol)
		rc = mcp_protocol_add_header_copy(&request.headers, "MCP-Protocol-Version",
						  client->protocol->version);

	const char *session = session_id ? session_id : client->session_id;
	if (rc == 0 && session)
		rc = mcp_protocol_add_header_copy(&request.headers, "Mcp-Session-Id", session);

	if (rc == 0)
		rc = mcp_http_exchange(&peer, &request, mcp_request_timeout_ms(client), &reply);

	if (rc == 0 && (reply.http_status < 200 || reply.http_status >= 300))
		rc = NB_ERR_PROTOCOL;

	return rc;
}

/* ---- Public HTTP transport functions ---- */

int mcp_http_connect(struct mcp_client *client)
{
	char *log_url;
	int rc;

	if (!client || !client->config.http_url[0])
		return NB_ERR_INVAL;

	struct http_control control = {.logger = client->config.logger};
	rc = http_client_open(&control, &client->http_client);
	if (rc < 0) {
		nb_log_write(client ? client->config.logger : NULL, NB_LOG_ERROR,
			     "mcp http: session init failed: %s",
			     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}

	free(client->session_id);
	client->session_id = NULL;

	log_url = mcp_http_redact_url_for_log(client->config.http_url);
	nb_log_write(client ? client->config.logger : NULL, NB_LOG_INFO,
		     "mcp http: connecting to '%s' at %s", client->config.name,
		     log_url ? log_url : client->config.http_url);
	free(log_url);
	return 0;
}

int mcp_http_terminate_session(struct mcp_client *client)
{
	if (!client || !client->http_client || !client->session_id || !client->protocol ||
	    client->protocol->per_request)
		return 0;

	struct mcp_wire_request request NB_CLEANUP(mcp_wire_request_cleanup) = {
		.id = -1, .http_method = "DELETE"};
	struct mcp_wire_reply reply NB_CLEANUP(mcp_wire_reply_cleanup) = {0};
	const char *header = NULL;
	nb_vec_init(&request.headers, sizeof(header));
	int rc = mcp_protocol_add_header_copy(&request.headers, "MCP-Protocol-Version",
					      client->protocol->version);
	if (rc == 0)
		rc = mcp_protocol_add_header_copy(&request.headers, "Mcp-Session-Id",
						  client->session_id);

	if (rc == 0) {
		/* Session cleanup has its own short budget after turn cancellation. */
		nb_http_client *backend NB_CLEANUP(http_client_cleanup) = NULL;
		rc = http_client_open(NULL, &backend);
		if (rc == 0) {
			struct mcp_client cleanup = {.config = client->config,
						     .protocol = client->protocol,
						     .http_client = backend};
			rc = mcp_http_exchange(&cleanup, &request, 200, &reply);
		}
	}

	if (rc == 0 && reply.http_status >= 400 && reply.http_status != 405 &&
	    reply.http_status != 404)
		rc = NB_ERR_PROTOCOL;

	return rc;
}

void mcp_http_disconnect(struct mcp_client *client)
{
	if (!client)
		return;
	nb_http_client_free(client->http_client);
	client->http_client = NULL;
	client->http_status = 0;
	free(client->session_id);
	client->session_id = NULL;
	client->connected = 0;
	nb_log_write(client ? client->config.logger : NULL, NB_LOG_INFO,
		     "mcp http: disconnected '%s'", client->config.name);
}
