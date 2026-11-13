#include "mcp/mcp_internal.h"
#include "cJSON.h"
#include "http/sse.h"
#include "nb/log.h"
#include "nb/buf.h"
#include "nb/encoding.h"
#include "nb/cleanup.h"
#include "errors.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MCP_AUTH_TOKEN_PLACEHOLDER "${auth_token}"

struct mcp_sse_extract {
	nb_buf data;
	int seen_data;
	int expected_id;
};

static void mcp_sse_extract_cleanup(struct mcp_sse_extract *ctx)
{
	nb_buf_cleanup(&ctx->data);
}

static int mcp_sse_extract_cb(const char *event, const char *data,
			      void *user_data)
{
	struct mcp_sse_extract *ctx = user_data;
	int rc;

	(void)event;
	if (!ctx || !data)
		return 0;
	if (ctx->expected_id >= 0) {
		cJSON *object = cJSON_Parse(data);
		cJSON *id = cJSON_GetObjectItemCaseSensitive(object, "id");
		cJSON *error = cJSON_GetObjectItemCaseSensitive(object, "error");
		int matches = (cJSON_IsNumber(id) && id->valuedouble == ctx->expected_id) ||
			(!id && error);

		cJSON_Delete(object);
		if (!matches)
			return 0;
	}
	nb_buf_clear(&ctx->data);
	rc = (nb_buf_append_cstr(&ctx->data, data) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	ctx->seen_data = 1;
	return 0;
}

static char *mcp_http_sse_response(const char *raw, size_t len, int expected_id)
{
	struct mcp_sse_extract ctx NB_CLEANUP(mcp_sse_extract_cleanup) = {0};
	struct sse_parser parser;
	char *json = NULL;
	int rc;

	if (!raw || len == 0)
		return NULL;

	rc = (nb_buf_init(&ctx.data), nb_buf_reserve(&ctx.data, 1024) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return NULL;
	ctx.seen_data = 0;
	ctx.expected_id = expected_id;

	sse_parser_init(&parser, mcp_sse_extract_cb, &ctx);
	rc = sse_parser_feed(&parser, raw, len);
	if (rc == 0)
		rc = sse_parser_feed(&parser, "\n", 1);
	sse_parser_free(&parser);

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

int mcp_http_exchange(struct mcp_client *client,
	const struct mcp_wire_request *request, int timeout_ms,
	struct mcp_wire_reply *reply)
{
	nb_vec headers NB_CLEANUP(nb_vec_cleanup) = {0};
	struct mcp_wire_reply received NB_CLEANUP(mcp_wire_reply_cleanup) = {0};
	nb_buf authorization NB_CLEANUP(nb_buf_cleanup) = {0};
	char *url NB_CLEANUP(nb_charp_cleanup) = NULL;
	int rc;

	if (!client || !request || !request->json || !reply || !client->session.initialized)
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
	long timeout = (long)(timeout_ms / 1000) + (timeout_ms % 1000 != 0);

	struct http_control control = {
		.logger = client->config.logger,
		.interrupt_check = client->interrupt_check,
		.interrupt_user_data = client->interrupt_user_data,
	};
	rc = http_session_post(&control, &client->session, url, request->json,
			       strlen(request->json), "application/json", ((void *)headers.data),
			       (int)headers.len, timeout);
	if (rc != 0)
		return rc;
	received.http_status = http_session_status(&client->session);
	received.session_id = http_session_header_copy(&client->session, "Mcp-Session-Id");
	const char *body = http_session_body_borrow(&client->session, NULL);

	if (body && body[0]) {
		received.json = mcp_http_sse_response(body, strlen(body), request->id);
		if (!received.json)
			received.json = strdup(body);
		if (!received.json)
			rc = (NB_ERR_NOMEM);
	}

	if (rc == 0) {
		*reply = received;
		memset(&received, 0, sizeof(received));
	}
	return rc;
}

/* ---- Public HTTP transport functions ---- */

int mcp_http_connect(struct mcp_client *client)
{
	char *log_url;
	int rc;

	if (!client || !client->config.http_url[0])
		return NB_ERR_INVAL;

	rc = http_session_init(client ? client->config.logger : NULL, &client->session);
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

void mcp_http_disconnect(struct mcp_client *client)
{
	if (!client)
		return;
	http_session_cleanup(&client->session);
	free(client->session_id);
	client->session_id = NULL;
	client->connected = 0;
	nb_log_write(client ? client->config.logger : NULL, NB_LOG_INFO,
		     "mcp http: disconnected '%s'", client->config.name);
}
