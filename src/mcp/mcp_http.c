#include "mcp/mcp_internal.h"
#include "cJSON.h"
#include "http/sse.h"
#include "util/log.h"
#include "util/buf.h"
#include "util/error.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MCP_AUTH_TOKEN_PLACEHOLDER "${auth_token}"

struct mcp_sse_extract {
	morph_buf_t data;
	int seen_data;
	int expected_id;
};

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
	morph_buf_reset(&ctx->data);
	rc = morph_buf_puts(&ctx->data, data);
	if (rc != 0)
		return rc;
	ctx->seen_data = 1;
	return 0;
}

static char *mcp_http_sse_response(const char *raw, size_t len, int expected_id)
{
	struct mcp_sse_extract ctx;
	struct sse_parser parser;
	char *json = NULL;
	int rc;

	if (!raw || len == 0)
		return NULL;

	rc = morph_buf_init(&ctx.data, 1024);
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
		json = morph_buf_detach(&ctx.data);
	else
		morph_buf_cleanup(&ctx.data);
	return json;
}

char *mcp_http_extract_sse_json(const char *raw, size_t len)
{
	return mcp_http_sse_response(raw, len, -1);
}

int mcp_http_url_uses_auth_token(const char *url)
{
	return url && strstr(url, MCP_AUTH_TOKEN_PLACEHOLDER) != NULL;
}

static int mcp_http_url_encode_append(morph_buf_t *buf, const char *src)
{
	static const char hex[] = "0123456789ABCDEF";

	if (!buf || !src)
		return -EINVAL;

	while (*src) {
		unsigned char ch = (unsigned char)*src;

		if ((ch >= 'A' && ch <= 'Z') ||
		    (ch >= 'a' && ch <= 'z') ||
		    (ch >= '0' && ch <= '9') ||
		    ch == '-' || ch == '.' || ch == '_' || ch == '~') {
			int rc = morph_buf_putc(buf, (char)ch);
			if (rc < 0)
				return rc;
		} else {
			int rc = morph_buf_putc(buf, '%');
			if (rc < 0)
				return rc;
			rc = morph_buf_putc(buf, hex[ch >> 4]);
			if (rc < 0)
				return rc;
			rc = morph_buf_putc(buf, hex[ch & 0x0f]);
			if (rc < 0)
				return rc;
		}
		src++;
	}

	return 0;
}

int mcp_http_build_request_url(const struct mcp_server_config *cfg,
			       char **out_url)
{
	const char *url;
	const char *token;
	const char *p;
	const char *match;
	morph_buf_t buf;
	size_t placeholder_len = strlen(MCP_AUTH_TOKEN_PLACEHOLDER);
	int rc;

	if (!cfg || !out_url)
		return -EINVAL;
	*out_url = NULL;

	url = cfg->http_url;
	if (!url || !url[0])
		return -EINVAL;

	if (!mcp_http_url_uses_auth_token(url)) {
		*out_url = strdup(url);
		if (!*out_url)
			return -ENOMEM;
		return 0;
	}

	if (!cfg->http_auth_token_env[0]) {
		log_err(cfg ? cfg->logger : NULL,
			"mcp http: URL uses %s but auth_token_env is not set",
			MCP_AUTH_TOKEN_PLACEHOLDER);
		return MORPH_ERR_NOT_CONFIGURED;
	}

	token = getenv(cfg->http_auth_token_env);
	if (!token || !token[0]) {
		log_err(cfg ? cfg->logger : NULL,
			"mcp http: URL uses %s but env var '%s' is not set",
			MCP_AUTH_TOKEN_PLACEHOLDER, cfg->http_auth_token_env);
		return MORPH_ERR_NOT_CONFIGURED;
	}

	rc = morph_buf_init(&buf, strlen(url) + strlen(token));
	if (rc < 0)
		return rc;

	p = url;
	while ((match = strstr(p, MCP_AUTH_TOKEN_PLACEHOLDER)) != NULL) {
		rc = morph_buf_append(&buf, p, (size_t)(match - p));
		if (rc < 0)
			goto out;
		rc = mcp_http_url_encode_append(&buf, token);
		if (rc < 0)
			goto out;
		p = match + placeholder_len;
	}
	rc = morph_buf_puts(&buf, p);
	if (rc < 0)
		goto out;

	*out_url = morph_buf_detach(&buf);
	if (!*out_url)
		rc = -ENOMEM;

out:
	if (rc < 0)
		morph_buf_cleanup(&buf);
	return rc;
}

static int mcp_http_redact_query_value(morph_buf_t *buf, const char *start,
				       const char *end)
{
	const char *eq = memchr(start, '=', (size_t)(end - start));
	int rc;

	if (!eq)
		return morph_buf_append(buf, start, (size_t)(end - start));

	rc = morph_buf_append(buf, start, (size_t)(eq - start + 1));
	if (rc < 0)
		return rc;
	if (eq + 1 < end)
		return morph_buf_puts(buf, "***");
	return 0;
}

static char *mcp_http_redact_url_for_log(const char *url)
{
	const char *q;
	const char *p;
	morph_buf_t buf;
	int rc;

	if (!url)
		return NULL;

	q = strchr(url, '?');
	if (!q)
		return strdup(url);

	rc = morph_buf_init(&buf, strlen(url));
	if (rc < 0)
		return NULL;

	rc = morph_buf_append(&buf, url, (size_t)(q - url + 1));
	if (rc < 0)
		goto out;

	p = q + 1;
	while (*p && *p != '#') {
		const char *end = p;

		while (*end && *end != '&' && *end != '#')
			end++;
		rc = mcp_http_redact_query_value(&buf, p, end);
		if (rc < 0)
			goto out;
		if (*end == '&') {
			rc = morph_buf_putc(&buf, '&');
			if (rc < 0)
				goto out;
			p = end + 1;
		} else {
			p = end;
		}
	}
	if (*p == '#') {
		rc = morph_buf_puts(&buf, p);
		if (rc < 0)
			goto out;
	}

	return morph_buf_detach(&buf);

out:
	morph_buf_cleanup(&buf);
	return NULL;
}

int mcp_http_exchange(struct mcp_client *client,
	const struct mcp_wire_request *request, int timeout_ms,
	struct mcp_wire_reply *reply)
{
	morph_array_t headers;
	morph_buf_t authorization = {0};
	char *url = NULL;
	int rc;

	if (!client || !client->session.initialized)
		return -EINVAL;
	if (request->headers.nelts > INT_MAX - 2)
		return -EOVERFLOW;
	rc = morph_array_init(&headers, request->headers.nelts + 2, sizeof(url));
	if (rc != 0)
		return rc;
	const char **values = morph_array_push_n(&headers, request->headers.nelts + 2);

	if (!values) {
		morph_array_cleanup(&headers);
		return -ENOMEM;
	}
	values[0] = "Accept: application/json, text/event-stream";
	for (size_t i = 0; i < request->headers.nelts; i++)
		values[i + 1] = *(const char **)morph_array_get(&request->headers, i);
	headers.nelts = request->headers.nelts + 1;
	if (client->config.http_auth_token_env[0] &&
	    !mcp_http_url_uses_auth_token(client->config.http_url)) {
		const char *token = getenv(client->config.http_auth_token_env);

		if (!token || !token[0]) {
			rc = (MORPH_ERR_NOT_CONFIGURED);
			goto out_free_headers;
		}
		rc = morph_buf_init(&authorization, 128);
		if (rc == 0)
			rc = morph_buf_printf(&authorization, "Authorization: Bearer %s", token);
		if (rc != 0)
			goto out_free_headers;
		values[headers.nelts++] = morph_buf_cstr(&authorization);
	}
	if (headers.nelts > INT_MAX) {
		rc = (-EOVERFLOW);
		goto out_free_headers;
	}
	rc = mcp_http_build_request_url(&client->config, &url);
	if (rc != 0)
		goto out_free_headers;
	long timeout = (long)(timeout_ms / 1000) + (timeout_ms % 1000 != 0);

	struct http_control control = {
		.logger = client->config.logger,
		.interrupt_check = client->interrupt_check,
		.interrupt_user_data = client->interrupt_user_data,
	};
	rc = http_session_post(&control, &client->session, url, request->json,
			       strlen(request->json), "application/json", headers.elts,
			       (int)headers.nelts, timeout);
	if (rc != 0)
		goto out_free_headers;
	reply->http_status = http_session_status(&client->session);
	reply->session_id = http_session_header_get(&client->session, "Mcp-Session-Id");
	const char *body = http_session_body(&client->session, NULL);

	if (body && body[0]) {
		reply->json = mcp_http_sse_response(body, strlen(body), request->id);
		if (!reply->json)
			reply->json = strdup(body);
		if (!reply->json)
			rc = (-ENOMEM);
	}

out_free_headers:
	free(url);
	morph_buf_cleanup(&authorization);
	morph_array_cleanup(&headers);
	return rc;
}

/* ---- Public HTTP transport functions ---- */

int mcp_http_connect(struct mcp_client *client)
{
	char *log_url;
	int rc;

	if (!client || !client->config.http_url[0])
		return -EINVAL;

	rc = http_session_init(client ? client->config.logger : NULL, &client->session);
	if (rc < 0) {
		log_err(client ? client->config.logger : NULL, "mcp http: session init failed: %s",
			morph_strerror(rc));
		return rc;
	}

	free(client->session_id);
	client->session_id = NULL;

	log_url = mcp_http_redact_url_for_log(client->config.http_url);
	log_info(client ? client->config.logger : NULL, "mcp http: connecting to '%s' at %s",
		 client->config.name, log_url ? log_url : client->config.http_url);
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
	log_info(client ? client->config.logger : NULL, "mcp http: disconnected '%s'",
		 client->config.name);
}
