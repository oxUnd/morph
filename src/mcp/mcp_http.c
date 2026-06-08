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

static int mcp_http_do_post(struct mcp_client *client, const char *body,
			    const char **extra_hdrs, int extra_count,
			    long timeout)
{
	const char *hdrs[8];
	int nhdrs = 0;
	char ver_hdr[128];
	char auth_hdr[512];
	char sess_hdr[256];

	hdrs[nhdrs++] = "Accept: application/json, text/event-stream";

	snprintf(ver_hdr, sizeof(ver_hdr),
		 "MCP-Protocol-Version: %s", client->negotiated_version[0]
		 ? client->negotiated_version : MCP_PROTOCOL_VERSION);
	hdrs[nhdrs++] = ver_hdr;

	if (client->config.http_auth_token_env[0]) {
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

	return http_session_post(&client->session, client->config.http_url,
				body, strlen(body), "application/json",
				hdrs, nhdrs, timeout);
}

/* ---- Public HTTP transport functions ---- */

int mcp_http_connect(struct mcp_client *client)
{
	if (!client || !client->config.http_url[0])
		return -EINVAL;

	int rc = http_session_init(&client->session);
	if (rc < 0) {
		log_err("mcp http: session init failed: %s", morph_strerror(rc));
		return rc;
	}

	client->session_id[0] = '\0';

	log_info("mcp http: connecting to '%s' at %s",
		 client->config.name, client->config.http_url);
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
	if (!client || !client->session.initialized)
		return -EINVAL;

	char *req = mcp_build_request(client->next_req_id++, method, params_json);
	if (!req)
		return -ENOMEM;

	int rc = mcp_http_do_post(client, req, NULL, 0, MCP_HTTP_TIMEOUT_S);
	free(req);

	if (rc < 0) {
		log_err("mcp http: request '%s' failed: %s", method, morph_strerror(rc));
		return rc;
	}

	long http_code = http_session_status(&client->session);
	if (http_code >= 400) {
		log_err("mcp http: request '%s' HTTP error %ld", method, http_code);
		MORPH_RETURN(MORPH_ERR_API);
	}

	const char *resp_raw = http_session_body(&client->session, NULL);
	if (!resp_raw) {
		log_err("mcp http: request '%s' returned empty body", method);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}

	char *resp_json = mcp_http_extract_sse_json(resp_raw, strlen(resp_raw));
	if (resp_json) {
		rc = mcp_parse_result(resp_json, out_result);
		free(resp_json);
	} else {
		char *raw_copy = strdup(resp_raw);
		if (!raw_copy)
			MORPH_RETURN(-ENOMEM);
		rc = mcp_parse_result(raw_copy, out_result);
		free(raw_copy);
	}
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