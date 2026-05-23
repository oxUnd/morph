#include "mcp/mcp.h"
#include "cJSON.h"
#include "util/log.h"
#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *mcp_build_request(int id, const char *method, const char *params_json);
int mcp_parse_result(const char *resp_json, char **out_result);

#define MCP_HTTP_TIMEOUT_S 30

/* ----- curl write callback ----- */

struct http_response_ctx {
	char *data;
	size_t size;
};

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct http_response_ctx *ctx = (struct http_response_ctx *)userdata;
	size_t total = size * nmemb;

	char *new_data = realloc(ctx->data, ctx->size + total + 1);
	if (!new_data)
		return 0;

	ctx->data = new_data;
	memcpy(ctx->data + ctx->size, ptr, total);
	ctx->size += total;
	ctx->data[ctx->size] = '\0';
	return total;
}

/* ----- SSE parser ----- */

struct sse_parser {
	char buf[MCP_JSON_BUF_MAX];
	size_t pos;
};

static void sse_parser_init(struct sse_parser *p)
{
	p->pos = 0;
	p->buf[0] = '\0';
}

static char *sse_parser_feed(struct sse_parser *p, const char *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (data[i] == '\n') {
			if (p->pos > 0) {
				p->buf[p->pos] = '\0';
				char *msg = strdup(p->buf);
				p->pos = 0;
				return msg;
			}
		} else if (data[i] != '\r') {
			if (p->pos < sizeof(p->buf) - 1)
				p->buf[p->pos++] = data[i];
		}
	}
	return NULL;
}

/* ----- HTTP POST helper ----- */

static int mcp_http_do_post(struct mcp_client *client, const char *body,
			    char **out_response)
{
	CURL *curl = client->curl_handle;
	if (!curl)
		return -EINVAL;

	struct http_response_ctx resp_ctx = { .data = NULL, .size = 0 };

	curl_easy_setopt(curl, CURLOPT_URL, client->config.http_url);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "Accept: application/json, text/event-stream");
	char version_header[128];
	snprintf(version_header, sizeof(version_header),
		 "MCP-Protocol-Version: %s", client->negotiated_version[0]
		 ? client->negotiated_version : MCP_PROTOCOL_VERSION);
	headers = curl_slist_append(headers, version_header);

	if (client->config.http_auth_token_env[0]) {
		const char *token_env = getenv(client->config.http_auth_token_env);
		if (token_env) {
			char auth[512];
			snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token_env);
			headers = curl_slist_append(headers, auth);
		}
	}

	if (client->session_id[0]) {
		char session_header[256];
		snprintf(session_header, sizeof(session_header),
			 "Mcp-Session-Id: %s", client->session_id);
		headers = curl_slist_append(headers, session_header);
	}

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_ctx);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)MCP_HTTP_TIMEOUT_S);

	CURLcode res = curl_easy_perform(curl);
	curl_slist_free_all(headers);

	if (res != CURLE_OK) {
		log_err("mcp http: request failed: %s", curl_easy_strerror(res));
		free(resp_ctx.data);
		return -EIO;
	}

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	if (http_code == 404 || http_code == 405) {
		log_err("mcp http: server returned %ld", http_code);
		free(resp_ctx.data);
		return -ENOTSUP;
	}

	if (http_code >= 400) {
		log_err("mcp http: HTTP error %ld", http_code);
		free(resp_ctx.data);
		return -EIO;
	}

	if (out_response)
		*out_response = resp_ctx.data;
	else
		free(resp_ctx.data);

	return 0;
}

/* Strip SSE envelope: extract JSON from "event: message\ndata: <json>\n" */
static char *mcp_http_extract_sse_json(const char *raw, size_t len)
{
	/* Look for "data: " prefix */
	const char *data_prefix = "data: ";
	const char *data_start = strstr(raw, data_prefix);
	if (!data_start)
		return NULL;
	data_start += strlen(data_prefix);

	/* Find end of line (first \n or \r after data) */
	const char *end = data_start;
	while ((size_t)(end - raw) < len && *end != '\n' && *end != '\r')
		end++;

	size_t json_len = (size_t)(end - data_start);
	char *json = malloc(json_len + 1);
	if (!json)
		return NULL;
	memcpy(json, data_start, json_len);
	json[json_len] = '\0';
	return json;
}

/* ---- Public HTTP transport functions ---- */

int mcp_http_connect(struct mcp_client *client)
{
	if (!client || !client->config.http_url[0])
		return -EINVAL;

	CURL *curl = curl_easy_init();
	if (!curl) {
		log_err("mcp http: curl_easy_init failed");
		return -ENOMEM;
	}

	client->curl_handle = curl;
	client->session_id[0] = '\0';

	log_info("mcp http: connecting to '%s' at %s",
		 client->config.name, client->config.http_url);
	return 0;
}

int mcp_http_initialize(struct mcp_client *client)
{
	if (!client || !client->curl_handle)
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

	char *resp_raw = NULL;
	int rc = mcp_http_do_post(client, req, &resp_raw);
	free(req);

	if (rc < 0 || !resp_raw) {
		log_err("mcp http: initialize failed");
		free(resp_raw);
		return rc ? rc : -EIO;
	}

	/* Handle SSE-wrapped response */
	char *resp_json = mcp_http_extract_sse_json(resp_raw, strlen(resp_raw));
	if (!resp_json)
		resp_json = resp_raw;
	else
		free(resp_raw);

	/* Parse initialize response */
	char *result_str = NULL;
	int parse_rc = mcp_parse_result(resp_json, &result_str);
	if (parse_rc < 0) {
		log_err("mcp http: initialize response had error");
		free(resp_json);
		return parse_rc;
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
	free(resp_json);

	log_info("mcp http: initialized '%s' (server=%s v%s, proto=%s)",
		 client->config.name,
		 client->server_name, client->server_version,
		 client->negotiated_version);

	/* Send initialized notification */
	char notif[64];
	snprintf(notif, sizeof(notif),
		 "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
	char *nresp = NULL;
	mcp_http_do_post(client, notif, &nresp);
	free(nresp);

	client->connected = 1;
	return 0;
}

int mcp_http_request(struct mcp_client *client, const char *method,
		     const char *params_json, char **out_result)
{
	if (!client || !client->curl_handle)
		return -EINVAL;

	char *req = mcp_build_request(client->next_req_id++, method, params_json);
	if (!req)
		return -ENOMEM;

	char *resp_raw = NULL;
	int rc = mcp_http_do_post(client, req, &resp_raw);
	free(req);

	if (rc < 0 || !resp_raw) {
		log_err("mcp http: request '%s' failed", method);
		free(resp_raw);
		return rc ? rc : -EIO;
	}

	/* Handle SSE-wrapped responses */
	char *resp_json = mcp_http_extract_sse_json(resp_raw, strlen(resp_raw));
	if (resp_json) {
		rc = mcp_parse_result(resp_json, out_result);
		free(resp_json);
	} else {
		rc = mcp_parse_result(resp_raw, out_result);
	}
	free(resp_raw);
	return rc;
}

void mcp_http_disconnect(struct mcp_client *client)
{
	if (!client)
		return;
	if (client->curl_handle) {
		curl_easy_cleanup(client->curl_handle);
		client->curl_handle = NULL;
	}
	client->connected = 0;
	log_info("mcp http: disconnected '%s'", client->config.name);
}
