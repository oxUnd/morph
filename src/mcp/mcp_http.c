#include "mcp/mcp.h"
#include "cJSON.h"
#include "http/sse.h"
#include "util/log.h"
#include "util/buf.h"
#include "util/error.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *mcp_build_request(int id, const char *method, const char *params_json);
int mcp_parse_result(const char *resp_json, char **out_result);

#define MCP_HTTP_TIMEOUT_S 30
#define MCP_AUTH_TOKEN_PLACEHOLDER "${auth_token}"

struct mcp_sse_extract {
	morph_buf_t data;
	int seen_data;
};

static int mcp_sse_extract_cb(const char *event, const char *data,
			      void *user_data)
{
	struct mcp_sse_extract *ctx = user_data;
	int rc;

	(void)event;
	if (!ctx || !data)
		return 0;
	if (ctx->seen_data) {
		rc = morph_buf_putc(&ctx->data, '\n');
		if (rc != 0)
			return rc;
	}
	rc = morph_buf_puts(&ctx->data, data);
	if (rc != 0)
		return rc;
	ctx->seen_data = 1;
	return 0;
}

char *mcp_http_extract_sse_json(const char *raw, size_t len)
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

int mcp_http_url_uses_auth_token(const char *url)
{
	return url && strstr(url, MCP_AUTH_TOKEN_PLACEHOLDER) != NULL;
}

static int mcp_http_url_encode_append(morph_buf_t *buf, const char *src)
{
	static const char hex[] = "0123456789ABCDEF";

	if (!buf || !src)
		MORPH_RETURN(-EINVAL);

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
		MORPH_RETURN(-EINVAL);
	*out_url = NULL;

	url = cfg->http_url;
	if (!url || !url[0])
		MORPH_RETURN(-EINVAL);

	if (!mcp_http_url_uses_auth_token(url)) {
		*out_url = strdup(url);
		if (!*out_url)
			MORPH_RETURN(-ENOMEM);
		return 0;
	}

	if (!cfg->http_auth_token_env[0]) {
		log_err("mcp http: URL uses %s but auth_token_env is not set",
			MCP_AUTH_TOKEN_PLACEHOLDER);
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	}

	token = getenv(cfg->http_auth_token_env);
	if (!token || !token[0]) {
		log_err("mcp http: URL uses %s but env var '%s' is not set",
			MCP_AUTH_TOKEN_PLACEHOLDER, cfg->http_auth_token_env);
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
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

static int mcp_http_do_post(struct mcp_client *client, const char *body,
			    const char **extra_hdrs, int extra_count,
			    long timeout)
{
	const char *hdrs[8];
	int nhdrs = 0;
	char ver_hdr[128];
	char auth_hdr[512];
	char sess_hdr[256];
	char *request_url = NULL;
	int url_auth;
	int rc;

	hdrs[nhdrs++] = "Accept: application/json, text/event-stream";

	snprintf(ver_hdr, sizeof(ver_hdr),
		 "MCP-Protocol-Version: %s", client->negotiated_version[0]
		 ? client->negotiated_version : MCP_PROTOCOL_VERSION);
	hdrs[nhdrs++] = ver_hdr;

	url_auth = mcp_http_url_uses_auth_token(client->config.http_url);
	if (client->config.http_auth_token_env[0] && !url_auth) {
		const char *token_env = getenv(client->config.http_auth_token_env);
		if (token_env) {
			snprintf(auth_hdr, sizeof(auth_hdr),
				 "Authorization: Bearer %s", token_env);
			hdrs[nhdrs++] = auth_hdr;
		}
	}

	if (client->session_id[0]) {
		snprintf(sess_hdr, sizeof(sess_hdr),
			 "Mcp-Session-Id: %s", client->session_id);
		hdrs[nhdrs++] = sess_hdr;
	}

	for (int i = 0; i < extra_count && nhdrs < 8; i++)
		hdrs[nhdrs++] = extra_hdrs[i];

	rc = mcp_http_build_request_url(&client->config, &request_url);
	if (rc < 0)
		return rc;

	rc = http_session_post(&client->session, request_url,
			       body, strlen(body), "application/json",
			       hdrs, nhdrs, timeout);
	free(request_url);
	return rc;
}

/* ---- Public HTTP transport functions ---- */

int mcp_http_connect(struct mcp_client *client)
{
	char *log_url;
	int rc;

	if (!client || !client->config.http_url[0])
		return -EINVAL;

	rc = http_session_init(&client->session);
	if (rc < 0) {
		log_err("mcp http: session init failed: %s", morph_strerror(rc));
		return rc;
	}

	client->session_id[0] = '\0';

	log_url = mcp_http_redact_url_for_log(client->config.http_url);
	log_info("mcp http: connecting to '%s' at %s",
		 client->config.name, log_url ? log_url : client->config.http_url);
	free(log_url);
	return 0;
}

int mcp_http_initialize(struct mcp_client *client)
{
	if (!client || !client->session.initialized)
		return -EINVAL;

	char params_buf[2048];
	snprintf(params_buf, sizeof(params_buf),
		 "{"
		 "\"protocolVersion\":\"%s\","
		 "\"capabilities\":{\"roots\":{\"listChanged\":false}},"
		 "\"clientInfo\":{\"name\":\"morph\",\"version\":\"" MORPH_VERSION "\"}"
		 "}",
		 MCP_PROTOCOL_VERSION);

	char *req = mcp_build_request(client->next_req_id++, "initialize", params_buf);
	if (!req)
		return -ENOMEM;

	int rc = mcp_http_do_post(client, req, NULL, 0, MCP_HTTP_TIMEOUT_S);
	free(req);

	if (rc < 0) {
		log_err("mcp http: initialize failed: %s", morph_strerror(rc));
		return rc;
	}

	long http_code = http_session_status(&client->session);
	if (http_code == 404 || http_code == 405) {
		log_err("mcp http: server returned %ld", http_code);
		return -ENOTSUP;
	}
	if (http_code >= 400) {
		log_err("mcp http: HTTP error %ld", http_code);
		MORPH_RETURN(MORPH_ERR_API);
	}

	const char *resp_raw = http_session_body(&client->session, NULL);
	if (!resp_raw || !*resp_raw) {
		log_err("mcp http: initialize returned empty body");
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}

	char *resp_json = mcp_http_extract_sse_json(resp_raw, strlen(resp_raw));
	const char *parsed = resp_json ? resp_json : resp_raw;

	char *result_str = NULL;
	int parse_rc = mcp_parse_result(parsed, &result_str);
	if (parse_rc < 0) {
		log_err("mcp http: initialize response had error (parse_rc=%d), raw=%.200s",
			parse_rc, resp_raw ? resp_raw : "(null)");
		free(resp_json);
		MORPH_RETURN(parse_rc < -256 ? parse_rc : MORPH_ERR_PARSE);
	}

	cJSON *obj = cJSON_Parse(result_str);
	if (obj) {
		cJSON *v = cJSON_GetObjectItem(obj, "protocolVersion");
		if (v && cJSON_IsString(v))
			strncpy(client->negotiated_version, v->valuestring,
				sizeof(client->negotiated_version) - 1);

		v = cJSON_GetObjectItem(obj, "serverInfo");
		if (v) {
			cJSON *sn = cJSON_GetObjectItem(v, "name");
			if (sn && cJSON_IsString(sn))
				strncpy(client->server_name, sn->valuestring,
					sizeof(client->server_name) - 1);
			cJSON *sv = cJSON_GetObjectItem(v, "version");
			if (sv && cJSON_IsString(sv))
				strncpy(client->server_version, sv->valuestring,
					sizeof(client->server_version) - 1);
		}

		cJSON *caps = cJSON_GetObjectItem(obj, "capabilities");
		if (caps) {
			client->supports_tools = cJSON_HasObjectItem(caps, "tools");
			client->supports_resources = cJSON_HasObjectItem(caps, "resources");
			client->supports_prompts = cJSON_HasObjectItem(caps, "prompts");
		}

		cJSON_Delete(obj);
	}
	free(result_str);

	char *sid = http_session_header_get(&client->session, "Mcp-Session-Id");
	if (sid) {
		strncpy(client->session_id, sid, sizeof(client->session_id) - 1);
		client->session_id[sizeof(client->session_id) - 1] = '\0';
		free(sid);
	}

	free(resp_json);

	log_info("mcp http: initialized '%s' (server=%s v%s, proto=%s)",
		 client->config.name,
		 client->server_name, client->server_version,
		 client->negotiated_version);

	/* Send initialized notification */
	char notif[64];
	snprintf(notif, sizeof(notif),
		 "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
	mcp_http_do_post(client, notif, NULL, 0, MCP_HTTP_TIMEOUT_S);

	client->connected = 1;
	return 0;
}

int mcp_http_request(struct mcp_client *client, const char *method,
		     const char *params_json, char **out_result)
{
	const char *resp_raw;
	char *resp_json;
	char *raw_copy;
	char *req;
	long http_code;
	int rc;

	if (!client || !client->session.initialized)
		return -EINVAL;

	pthread_mutex_lock(&client->lock);
	req = mcp_build_request(client->next_req_id++, method, params_json);
	if (!req) {
		pthread_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	rc = mcp_http_do_post(client, req, NULL, 0, MCP_HTTP_TIMEOUT_S);
	free(req);

	if (rc < 0) {
		log_err("mcp http: request '%s' failed: %s", method, morph_strerror(rc));
		pthread_mutex_unlock(&client->lock);
		return rc;
	}

	http_code = http_session_status(&client->session);
	if (http_code >= 400) {
		log_err("mcp http: request '%s' HTTP error %ld", method, http_code);
		pthread_mutex_unlock(&client->lock);
		MORPH_RETURN(MORPH_ERR_API);
	}

	resp_raw = http_session_body(&client->session, NULL);
	if (!resp_raw) {
		log_err("mcp http: request '%s' returned empty body", method);
		pthread_mutex_unlock(&client->lock);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}

	resp_json = mcp_http_extract_sse_json(resp_raw, strlen(resp_raw));
	if (resp_json) {
		rc = mcp_parse_result(resp_json, out_result);
		free(resp_json);
	} else {
		raw_copy = strdup(resp_raw);
		if (!raw_copy) {
			pthread_mutex_unlock(&client->lock);
			MORPH_RETURN(-ENOMEM);
		}
		rc = mcp_parse_result(raw_copy, out_result);
		free(raw_copy);
	}
	pthread_mutex_unlock(&client->lock);
	return rc;
}

void mcp_http_disconnect(struct mcp_client *client)
{
	if (!client)
		return;
	http_session_cleanup(&client->session);
	client->connected = 0;
	log_info("mcp http: disconnected '%s'", client->config.name);
}
