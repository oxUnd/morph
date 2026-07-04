#include "mcp/mcp.h"
#include "cJSON.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/log.h"
#include "util/error.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----- Transport-level declarations ----- */

int mcp_stdio_request(struct mcp_client *client, int req_id,
		      const char *method, const char *params_json,
		      char **out_result);
int mcp_stdio_connect(struct mcp_client *client);
int mcp_stdio_initialize(struct mcp_client *client);
void mcp_stdio_disconnect(struct mcp_client *client);
int mcp_stdio_ping(struct mcp_client *client);

int mcp_http_connect(struct mcp_client *client);
int mcp_http_initialize(struct mcp_client *client);
void mcp_http_disconnect(struct mcp_client *client);
int mcp_http_request(struct mcp_client *client, const char *method,
		     const char *params_json, char **out_result);

static char *mcp_strdup_result(struct arena *arena, const char *s)
{
	if (!s)
		return NULL;
	if (arena)
		return arena_strdup(arena, s);
	return strdup(s);
}

static int mcp_next_request_id(struct mcp_client *client)
{
	int req_id;

	pthread_mutex_lock(&client->lock);
	req_id = client->next_req_id++;
	pthread_mutex_unlock(&client->lock);
	return req_id;
}

/* ----- Shared JSON-RPC helpers ----- */

char *mcp_build_request(int id, const char *method, const char *params_json)
{
	cJSON *req = cJSON_CreateObject();
	if (!req)
		return NULL;
	cJSON_AddStringToObject(req, "jsonrpc", "2.0");
	cJSON_AddNumberToObject(req, "id", (double)id);
	cJSON_AddStringToObject(req, "method", method);

	if (params_json && params_json[0]) {
		cJSON *params = cJSON_Parse(params_json);
		if (params)
			cJSON_AddItemToObject(req, "params", params);
	}

	char *out = cJSON_PrintUnformatted(req);
	cJSON_Delete(req);
	return out;
}

int mcp_parse_result(const char *resp_json, char **out_result)
{
	cJSON *obj = cJSON_Parse(resp_json);
	if (!obj) {
		log_err("mcp: failed to parse response JSON");
		MORPH_RETURN(MORPH_ERR_PARSE);
	}

	cJSON *err = cJSON_GetObjectItem(obj, "error");
	if (err) {
		int code = 0;
		const char *msg = "";
		cJSON *c = cJSON_GetObjectItem(err, "code");
		if (c && cJSON_IsNumber(c))
			code = (int)c->valuedouble;
		c = cJSON_GetObjectItem(err, "message");
		if (c && cJSON_IsString(c))
			msg = c->valuestring;
		log_err("mcp: JSON-RPC error: code=%d, message=%s", code, msg);
		if (out_result)
			*out_result = strdup(resp_json);
		cJSON_Delete(obj);
		return MORPH_ERR_PROTOCOL;
	}

	cJSON *result = cJSON_GetObjectItem(obj, "result");
	if (result && out_result)
		*out_result = cJSON_PrintUnformatted(result);

	cJSON_Delete(obj);
	return 0;
}

/* ----- Registry ----- */

void mcp_registry_init(struct mcp_registry *reg)
{
	if (!reg)
		return;
	memset(reg, 0, sizeof(*reg));
	(void)morph_strmap_init(&reg->by_name, MCP_MAX_SERVERS);
}

int mcp_registry_add(struct mcp_registry *reg, const struct mcp_server_config *cfg)
{
	if (!reg || !cfg || !cfg->name[0])
		return -EINVAL;
	if (reg->count >= MCP_MAX_SERVERS)
		return -ENOSPC;

	if (mcp_registry_get(reg, cfg->name))
		return -EEXIST;

	struct mcp_client *client = calloc(1, sizeof(*client));
	if (!client)
		return -ENOMEM;

	memcpy(&client->config, cfg, sizeof(*cfg));
	client->connected = 0;
	client->server_pid = -1;
	client->stdin_fd = -1;
	client->stdout_fd = -1;
	client->next_req_id = 1;
	pthread_mutex_init(&client->lock, NULL);

	reg->servers[reg->count] = client;
	(void)morph_strmap_set(&reg->by_name, client->config.name, client);
	reg->count++;

	log_info("mcp: registered server '%s' (transport=%s)",
		 cfg->name,
		 cfg->transport == MCP_TRANSPORT_STDIO ? "stdio" : "http");
	return 0;
}

struct mcp_client *mcp_registry_get(struct mcp_registry *reg, const char *name)
{
	struct mcp_client *client;

	if (!reg || !name)
		return NULL;
	client = (struct mcp_client *)morph_strmap_get(&reg->by_name, name);
	if (client)
		return client;
	for (int i = 0; i < reg->count; i++) {
		if (strcmp(reg->servers[i]->config.name, name) == 0)
			return reg->servers[i];
	}
	return NULL;
}

int mcp_registry_count(struct mcp_registry *reg)
{
	return reg ? reg->count : 0;
}

void mcp_registry_cleanup(struct mcp_registry *reg)
{
	if (!reg)
		return;
	for (int i = 0; i < reg->count; i++) {
		if (reg->servers[i]) {
			mcp_disconnect(reg->servers[i]);
			pthread_mutex_destroy(&reg->servers[i]->lock);
			free(reg->servers[i]);
		}
	}
	reg->count = 0;
	morph_strmap_cleanup(&reg->by_name);
}

/* ----- Lifecycle dispatch ----- */

int mcp_connect_stdio(struct mcp_client *client)
{
	return mcp_stdio_connect(client);
}

int mcp_connect_http(struct mcp_client *client)
{
	return mcp_http_connect(client);
}

int mcp_initialize(struct mcp_client *client)
{
	if (!client)
		return -EINVAL;
	if (client->config.transport == MCP_TRANSPORT_STDIO)
		return mcp_stdio_initialize(client);
	return mcp_http_initialize(client);
}

void mcp_disconnect(struct mcp_client *client)
{
	if (!client)
		return;
	if (client->config.transport == MCP_TRANSPORT_STDIO)
		mcp_stdio_disconnect(client);
	else
		mcp_http_disconnect(client);
}

/* ----- Lazy-connect wrapper ----- */

int mcp_ensure_connected(struct mcp_client *client)
{
	if (!client)
		return -EINVAL;

	pthread_mutex_lock(&client->lock);
	if (client->connected || client->connecting) {
		pthread_mutex_unlock(&client->lock);
		return client->connecting ? -EAGAIN : 0;
	}
	client->connecting = 1;
	pthread_mutex_unlock(&client->lock);

	int rc;
	if (client->config.transport == MCP_TRANSPORT_STDIO)
		rc = mcp_stdio_connect(client);
	else
		rc = mcp_http_connect(client);

	if (rc == 0)
		rc = mcp_initialize(client);

	pthread_mutex_lock(&client->lock);
	client->connecting = 0;
	if (rc == 0)
		client->connected = 1;
	pthread_mutex_unlock(&client->lock);

	return rc;
}

int mcp_ping(struct mcp_client *client)
{
	if (!client)
		return -EINVAL;
	if (client->config.transport == MCP_TRANSPORT_STDIO)
		return mcp_stdio_ping(client);

	/* HTTP ping */
	char *result = NULL;
	int rc = mcp_http_request(client, "ping", NULL, &result);
	free(result);
	return rc;
}

static void mcp_test_message(char *message, size_t message_cap,
			     const char *fmt, ...)
{
	va_list ap;

	if (!message || message_cap == 0)
		return;
	va_start(ap, fmt);
	vsnprintf(message, message_cap, fmt, ap);
	va_end(ap);
}

int mcp_test_connection(const struct mcp_server_config *cfg,
			char *message, size_t message_cap)
{
	struct mcp_registry reg;
	struct mcp_client *client;
	int rc;

	if (!cfg || !cfg->name[0]) {
		mcp_test_message(message, message_cap, "invalid server config");
		MORPH_RETURN(-EINVAL);
	}

	mcp_registry_init(&reg);
	rc = mcp_registry_add(&reg, cfg);
	if (rc < 0) {
		mcp_test_message(message, message_cap, "%s", morph_strerror(rc));
		mcp_registry_cleanup(&reg);
		return rc;
	}

	client = mcp_registry_get(&reg, cfg->name);
	rc = mcp_ensure_connected(client);
	if (rc == 0) {
		if (client && client->server_name[0]) {
			mcp_test_message(message, message_cap, "Connected to %s",
					 client->server_name);
		} else {
			mcp_test_message(message, message_cap, "Connected");
		}
	} else if (cfg->transport == MCP_TRANSPORT_STREAMABLE_HTTP &&
		   client && http_session_status(&client->session) >= 400) {
		mcp_test_message(message, message_cap, "HTTP %ld",
				 http_session_status(&client->session));
	} else {
		mcp_test_message(message, message_cap, "%s", morph_strerror(rc));
	}

	mcp_registry_cleanup(&reg);
	return rc;
}

/* ----- MCP tools/list ----- */

int mcp_list_tools(struct mcp_client *client, struct arena *arena,
		   struct mcp_tool_desc **out_tools, int *out_count)
{
	if (!client || !out_tools || !out_count || !arena)
		return -EINVAL;

	char *result = NULL;
	int req_id;
	int rc;

	if (client->config.transport == MCP_TRANSPORT_STDIO) {
		req_id = mcp_next_request_id(client);
		rc = mcp_stdio_request(client, req_id, "tools/list", NULL, &result);
	} else {
		rc = mcp_http_request(client, "tools/list", NULL, &result);
	}

	if (rc < 0 || !result) {
		free(result);
		return rc ? rc : MORPH_ERR_PROTOCOL;
	}

	cJSON *obj = cJSON_Parse(result);
	if (!obj) {
		log_err("mcp: tools/list response parse error");
		free(result);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}

	cJSON *tools_arr = cJSON_GetObjectItem(obj, "tools");
	if (!tools_arr || !cJSON_IsArray(tools_arr)) {
		cJSON_Delete(obj);
		free(result);
		*out_tools = NULL;
		*out_count = 0;
		return 0;
	}

	int count = cJSON_GetArraySize(tools_arr);
	struct mcp_tool_desc *tools = arena_alloc(arena, (size_t)count * sizeof(*tools));
	if (!tools) {
		cJSON_Delete(obj);
		free(result);
		return -ENOMEM;
	}

	for (int i = 0; i < count; i++) {
		cJSON *t = cJSON_GetArrayItem(tools_arr, i);
		cJSON *v;

		v = cJSON_GetObjectItem(t, "name");
		if (v && cJSON_IsString(v))
			strncpy(tools[i].name, v->valuestring, MCP_NAME_MAX - 1);

		v = cJSON_GetObjectItem(t, "title");
		if (v && cJSON_IsString(v))
			strncpy(tools[i].title, v->valuestring, MCP_NAME_MAX - 1);

		v = cJSON_GetObjectItem(t, "description");
		if (v && cJSON_IsString(v))
			strncpy(tools[i].description, v->valuestring, MCP_DESC_MAX - 1);

		v = cJSON_GetObjectItem(t, "inputSchema");
		if (v) {
			char *schema = cJSON_PrintUnformatted(v);
			if (schema) {
				strncpy(tools[i].input_schema, schema, MCP_SCHEMA_MAX - 1);
				free(schema);
			}
		}
	}

	cJSON_Delete(obj);
	free(result);

	*out_tools = tools;
	*out_count = count;
	return 0;
}

/* ----- MCP tools/call ----- */

int mcp_call_tool(struct mcp_client *client, struct arena *arena, const char *name,
		  const char *args_json, struct tool_result *out_result)
{
	if (!client || !name || !out_result)
		return -EINVAL;

	char params_buf[MCP_JSON_BUF_MAX];
	snprintf(params_buf, sizeof(params_buf),
		 "{\"name\":\"%s\",\"arguments\":%s}",
		 name, args_json ? args_json : "{}");

	char *result = NULL;
	int req_id;
	int rc;

	if (client->config.transport == MCP_TRANSPORT_STDIO) {
		req_id = mcp_next_request_id(client);
		rc = mcp_stdio_request(client, req_id, "tools/call", params_buf, &result);
	} else {
		rc = mcp_http_request(client, "tools/call", params_buf, &result);
	}

	if (rc < 0 || !result) {
		if (rc >= 0 && !result) {
			(void)tool_result_take_text(out_result, strdup("{\"error\":\"empty response\",\"isError\":true}"));
			MORPH_RETURN(MORPH_ERR_PROTOCOL);
		}
		(void)tool_result_printf(out_result,
					 "{\"error\":\"MCP call failed\","
					 "\"code\":%d,\"isError\":true}",
					 rc);
		free(result);
		return rc;
	}

	/* Parse the tools/call response: extract content array into a text string */
	cJSON *obj = cJSON_Parse(result);
	if (!obj) {
		log_err("mcp: tools/call response parse error");
		(void)tool_result_take_text(out_result, strdup("{\"error\":\"response parse error\",\"isError\":true}"));
		free(result);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}

	cJSON *is_err = cJSON_GetObjectItem(obj, "isError");
	int tool_error = (is_err && cJSON_IsBool(is_err) && cJSON_IsTrue(is_err));

	/* Extract text content */
	cJSON *content = cJSON_GetObjectItem(obj, "content");
	if (content && cJSON_IsArray(content)) {
		morph_buf_t combined_buf;
		int buf_ok = morph_buf_init(&combined_buf, 256);

		for (int i = 0; i < cJSON_GetArraySize(content); i++) {
			cJSON *item = cJSON_GetArrayItem(content, i);
			cJSON *type = cJSON_GetObjectItem(item, "type");
			if (type && cJSON_IsString(type)) {
				const char *text_to_add = NULL;
				if (strcmp(type->valuestring, "text") == 0) {
					cJSON *text = cJSON_GetObjectItem(item, "text");
					if (text && cJSON_IsString(text))
						text_to_add = text->valuestring;
				} else if (strcmp(type->valuestring, "image") == 0) {
					text_to_add = "[image content]";
				}

				if (text_to_add && buf_ok == 0) {
					if (combined_buf.len > 0)
						buf_ok = morph_buf_putc(&combined_buf, '\n');
					if (buf_ok == 0)
						buf_ok = morph_buf_puts(&combined_buf, text_to_add);
				}
			}
		}

		if (buf_ok == 0 && combined_buf.len > 0) {
			cJSON *resp = cJSON_CreateObject();
			cJSON_AddStringToObject(resp, "content",
						combined_buf.data);
			cJSON_AddBoolToObject(resp, "isError", tool_error);
			(void)tool_result_take_json(out_result, cJSON_PrintUnformatted(resp));
			cJSON_Delete(resp);
			morph_buf_cleanup(&combined_buf);
		} else {
			morph_buf_cleanup(&combined_buf);
			(void)tool_result_take_text(out_result, strdup("{\"error\":\"out of memory\",\"isError\":true}"));
		}
	} else {
		(void)tool_result_take_text(out_result, mcp_strdup_result(arena, result));
	}

	cJSON_Delete(obj);
	free(result);
	return tool_error ? -EIO : 0;
}

/* ----- MCP resources/list ----- */

int mcp_list_resources(struct mcp_client *client, struct arena *arena,
		       struct mcp_resource_desc **out_res, int *out_count)
{
	if (!client || !out_res || !out_count || !arena)
		return -EINVAL;

	char *result = NULL;
	int req_id;
	int rc;

	if (client->config.transport == MCP_TRANSPORT_STDIO) {
		req_id = mcp_next_request_id(client);
		rc = mcp_stdio_request(client, req_id, "resources/list", NULL, &result);
	} else {
		rc = mcp_http_request(client, "resources/list", NULL, &result);
	}

	if (rc < 0 || !result) {
		free(result);
		return rc ? rc : MORPH_ERR_PROTOCOL;
	}

	cJSON *obj = cJSON_Parse(result);
	if (!obj) {
		log_err("mcp: resources/list response parse error");
		free(result);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}

	cJSON *res_arr = cJSON_GetObjectItem(obj, "resources");
	if (!res_arr || !cJSON_IsArray(res_arr)) {
		cJSON_Delete(obj);
		free(result);
		*out_res = NULL;
		*out_count = 0;
		return 0;
	}

	int count = cJSON_GetArraySize(res_arr);
	struct mcp_resource_desc *res = arena_alloc(arena, (size_t)count * sizeof(*res));
	if (!res) {
		cJSON_Delete(obj);
		free(result);
		return -ENOMEM;
	}

	for (int i = 0; i < count; i++) {
		cJSON *r = cJSON_GetArrayItem(res_arr, i);
		cJSON *v;

		v = cJSON_GetObjectItem(r, "uri");
		if (v && cJSON_IsString(v))
			strncpy(res[i].uri, v->valuestring, MCP_URI_MAX - 1);

		v = cJSON_GetObjectItem(r, "name");
		if (v && cJSON_IsString(v))
			strncpy(res[i].name, v->valuestring, MCP_NAME_MAX - 1);

		v = cJSON_GetObjectItem(r, "description");
		if (v && cJSON_IsString(v))
			strncpy(res[i].description, v->valuestring, MCP_DESC_MAX - 1);

		v = cJSON_GetObjectItem(r, "mimeType");
		if (v && cJSON_IsString(v))
			strncpy(res[i].mime_type, v->valuestring, sizeof(res[i].mime_type) - 1);
	}

	cJSON_Delete(obj);
	free(result);

	*out_res = res;
	*out_count = count;
	return 0;
}

/* ----- MCP resources/read ----- */

int mcp_read_resource(struct mcp_client *client, struct arena *arena,
		      const char *uri, struct tool_result *out_content)
{
	if (!client || !uri || !out_content)
		return -EINVAL;

	char params_buf[MCP_JSON_BUF_MAX];
	snprintf(params_buf, sizeof(params_buf),
		 "{\"uri\":\"%s\"}", uri);

	char *result = NULL;
	int req_id;
	int rc;

	if (client->config.transport == MCP_TRANSPORT_STDIO) {
		req_id = mcp_next_request_id(client);
		rc = mcp_stdio_request(client, req_id, "resources/read", params_buf, &result);
	} else {
		rc = mcp_http_request(client, "resources/read", params_buf, &result);
	}

	if (rc < 0 || !result) {
		free(result);
		return rc ? rc : MORPH_ERR_PROTOCOL;
	}

	cJSON *obj = cJSON_Parse(result);
	if (!obj) {
		log_err("mcp: resources/read response parse error");
		free(result);
		return -EINVAL;
	}

	/* Extract text from contents array */
	cJSON *contents = cJSON_GetObjectItem(obj, "contents");
	if (contents && cJSON_IsArray(contents)) {
		char combined[MCP_JSON_BUF_MAX];
		combined[0] = '\0';
		size_t offset = 0;

		for (int i = 0; i < cJSON_GetArraySize(contents); i++) {
			cJSON *item = cJSON_GetArrayItem(contents, i);
			cJSON *text = cJSON_GetObjectItem(item, "text");
			if (text && cJSON_IsString(text)) {
				size_t remaining = sizeof(combined) - offset - 1;
				if (remaining > 0) {
					if (offset > 0)
						combined[offset++] = '\n';
					strncpy(combined + offset, text->valuestring, remaining);
					offset += strlen(combined + offset);
				}
			}
			cJSON *blob = cJSON_GetObjectItem(item, "blob");
			if (blob && cJSON_IsString(blob)) {
				size_t remaining = sizeof(combined) - offset - 1;
				if (remaining > strlen(blob->valuestring)) {
					if (offset > 0)
						combined[offset++] = '\n';
					strncpy(combined + offset, blob->valuestring, remaining);
					offset += strlen(combined + offset);
				}
			}
		}

		(void)tool_result_take_text(out_content, mcp_strdup_result(arena, combined));
	} else {
		(void)tool_result_take_text(out_content, mcp_strdup_result(arena, result));
	}

	cJSON_Delete(obj);
	free(result);
	return 0;
}

/* ----- MCP prompts/list ----- */

int mcp_list_prompts(struct mcp_client *client, struct arena *arena,
		     struct mcp_prompt_desc **out_prompts, int *out_count)
{
	if (!client || !out_prompts || !out_count || !arena)
		return -EINVAL;

	char *result = NULL;
	int req_id;
	int rc;

	if (client->config.transport == MCP_TRANSPORT_STDIO) {
		req_id = mcp_next_request_id(client);
		rc = mcp_stdio_request(client, req_id, "prompts/list", NULL, &result);
	} else {
		rc = mcp_http_request(client, "prompts/list", NULL, &result);
	}

	if (rc < 0 || !result) {
		free(result);
		return rc ? rc : MORPH_ERR_PROTOCOL;
	}

	cJSON *obj = cJSON_Parse(result);
	if (!obj) {
		log_err("mcp: prompts/list response parse error");
		free(result);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}

	cJSON *prompts_arr = cJSON_GetObjectItem(obj, "prompts");
	if (!prompts_arr || !cJSON_IsArray(prompts_arr)) {
		cJSON_Delete(obj);
		free(result);
		*out_prompts = NULL;
		*out_count = 0;
		return 0;
	}

	int count = cJSON_GetArraySize(prompts_arr);
	struct mcp_prompt_desc *prompts = arena_alloc(arena, (size_t)count * sizeof(*prompts));
	if (!prompts) {
		cJSON_Delete(obj);
		free(result);
		return -ENOMEM;
	}

	for (int i = 0; i < count; i++) {
		cJSON *p = cJSON_GetArrayItem(prompts_arr, i);
		cJSON *v;

		v = cJSON_GetObjectItem(p, "name");
		if (v && cJSON_IsString(v))
			strncpy(prompts[i].name, v->valuestring, MCP_NAME_MAX - 1);

		v = cJSON_GetObjectItem(p, "description");
		if (v && cJSON_IsString(v))
			strncpy(prompts[i].description, v->valuestring, MCP_DESC_MAX - 1);

		v = cJSON_GetObjectItem(p, "arguments");
		if (v && cJSON_IsArray(v)) {
			char *schema = cJSON_PrintUnformatted(v);
			if (schema) {
				strncpy(prompts[i].arguments_schema, schema, MCP_SCHEMA_MAX - 1);
				free(schema);
			}
		}
	}

	cJSON_Delete(obj);
	free(result);

	*out_prompts = prompts;
	*out_count = count;
	return 0;
}

/* ----- MCP prompts/get ----- */

int mcp_get_prompt(struct mcp_client *client, struct arena *arena, const char *name,
		   const char *args_json, struct tool_result *out_result)
{
	if (!client || !name || !out_result)
		return -EINVAL;

	char params_buf[MCP_JSON_BUF_MAX];
	if (args_json && args_json[0]) {
		snprintf(params_buf, sizeof(params_buf),
			 "{\"name\":\"%s\",\"arguments\":%s}", name, args_json);
	} else {
		snprintf(params_buf, sizeof(params_buf),
			 "{\"name\":\"%s\",\"arguments\":{}}", name);
	}

	char *result = NULL;
	int req_id;
	int rc;

	if (client->config.transport == MCP_TRANSPORT_STDIO) {
		req_id = mcp_next_request_id(client);
		rc = mcp_stdio_request(client, req_id, "prompts/get", params_buf, &result);
	} else {
		rc = mcp_http_request(client, "prompts/get", params_buf, &result);
	}

	if (rc < 0 || !result) {
		free(result);
		return rc ? rc : MORPH_ERR_PROTOCOL;
	}

	(void)tool_result_take_text(out_result, mcp_strdup_result(arena, result));
	free(result);
	return 0;
}

/* ----- morph tool registry integration ----- */

struct mcp_tool_binding {
	struct mcp_client *client;
	char mcp_tool_name[MCP_NAME_MAX];
};

static int mcp_tool_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct mcp_tool_binding *binding = (struct mcp_tool_binding *)user_data;
	if (!binding || !binding->client)
		return -EINVAL;

	int rc = mcp_ensure_connected(binding->client);
	if (rc < 0) {
		(void)tool_result_printf(result,
					 "{\"error\":\"MCP server '%s' not "
					 "connected\",\"code\":%d}",
					 binding->client->config.name, rc);
		return rc;
	}

	return mcp_call_tool(binding->client, NULL, binding->mcp_tool_name,
			     args_json, result);
}

int mcp_register_server_tools(struct mcp_client *client,
			      struct tool_registry *reg)
{
	if (!client || !reg)
		return -EINVAL;

	int rc = mcp_ensure_connected(client);
	if (rc < 0)
		return rc;
	if (!client->supports_tools)
		return 0;

	struct arena *arena = arena_create(0);
	if (!arena)
		return -ENOMEM;

	struct mcp_tool_desc *tools = NULL;
	int count = 0;
	rc = mcp_list_tools(client, arena, &tools, &count);
	if (rc < 0 || count == 0) {
		arena_destroy(arena);
		return rc;
	}

	int registered = 0;
	for (int i = 0; i < count; i++) {
		struct mcp_tool_binding *binding = calloc(1, sizeof(*binding));
		if (!binding) {
			log_err("mcp: failed to allocate tool binding");
			continue;
		}
		binding->client = client;
		strncpy(binding->mcp_tool_name, tools[i].name, MCP_NAME_MAX - 1);

		char tool_name[TOOL_NAME_MAX];
		snprintf(tool_name, sizeof(tool_name), "mcp_%s__%s",
			 client->config.name, tools[i].name);

		/* sanitize name */
		for (size_t j = 0; j < strlen(tool_name); j++) {
			char c = tool_name[j];
			if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			      (c >= '0' && c <= '9') || c == '_' || c == '-'))
				tool_name[j] = '_';
		}

		const char *schema = tools[i].input_schema;
		if (!schema || !schema[0])
			schema = "{\"type\":\"object\",\"additionalProperties\":false}";

		rc = tool_register(reg, tool_name, tools[i].description,
				   schema, mcp_tool_exec, binding,
				   (tool_user_data_destroy_fn)free);
		if (rc < 0) {
			log_warn("mcp: failed to register tool '%s' (rc=%d)", tool_name, rc);
			free(binding);
		} else {
			log_info("mcp: registered tool '%s' -> '%s'", tools[i].name, tool_name);
			registered++;
		}
	}

	arena_destroy(arena);
	return registered;
}

struct mcp_resource_binding {
	struct mcp_client *client;
	char uri[MCP_URI_MAX];
};

static int mcp_resource_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct mcp_resource_binding *binding = (struct mcp_resource_binding *)user_data;
	if (!binding || !binding->client)
		return -EINVAL;

	int rc = mcp_ensure_connected(binding->client);
	if (rc < 0) {
		(void)tool_result_printf(result,
					 "{\"error\":\"MCP server '%s' not "
					 "connected\",\"code\":%d}",
					 binding->client->config.name, rc);
		return rc;
	}

	(void)args_json;
	return mcp_read_resource(binding->client, NULL, binding->uri, result);
}

int mcp_register_server_resources(struct mcp_client *client,
				  struct tool_registry *reg)
{
	if (!client || !reg)
		return -EINVAL;

	int rc = mcp_ensure_connected(client);
	if (rc < 0)
		return rc;
	if (!client->supports_resources)
		return 0;

	struct arena *arena = arena_create(0);
	if (!arena)
		return -ENOMEM;

	struct mcp_resource_desc *res = NULL;
	int count = 0;
	rc = mcp_list_resources(client, arena, &res, &count);
	if (rc < 0 || count == 0) {
		arena_destroy(arena);
		return rc;
	}

	int registered = 0;
	for (int i = 0; i < count; i++) {
		struct mcp_resource_binding *binding = calloc(1, sizeof(*binding));
		if (!binding) {
			log_err("mcp: failed to allocate resource binding");
			continue;
		}
		binding->client = client;
		strncpy(binding->uri, res[i].uri, MCP_URI_MAX - 1);

		char tool_name[TOOL_NAME_MAX];
		snprintf(tool_name, sizeof(tool_name), "mcp_res_%s__%s",
			 client->config.name, res[i].name);

		for (size_t j = 0; j < strlen(tool_name); j++) {
			char c = tool_name[j];
			if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			      (c >= '0' && c <= '9') || c == '_' || c == '-'))
				tool_name[j] = '_';
		}

		char desc_buf[TOOL_DESC_MAX];
		snprintf(desc_buf, sizeof(desc_buf), "Read MCP resource: %s (%s)",
			 res[i].name, res[i].uri);

		rc = tool_register(reg, tool_name, desc_buf,
				   "{\"type\":\"object\",\"additionalProperties\":false}",
				   mcp_resource_exec, binding,
				   (tool_user_data_destroy_fn)free);
		if (rc < 0) {
			log_warn("mcp: failed to register resource '%s' (rc=%d)", tool_name, rc);
			free(binding);
		} else {
			struct tool_entry *e = tool_lookup(reg, tool_name);
			if (e)
				e->flags |= TOOL_FLAG_READONLY;
			log_info("mcp: registered resource '%s' -> '%s'", res[i].name, tool_name);
			registered++;
		}
	}

	arena_destroy(arena);
	return registered;
}

struct mcp_prompt_binding {
	struct mcp_client *client;
	char mcp_prompt_name[MCP_NAME_MAX];
};

static int mcp_prompt_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct mcp_prompt_binding *binding = (struct mcp_prompt_binding *)user_data;
	if (!binding || !binding->client)
		return -EINVAL;

	int rc = mcp_ensure_connected(binding->client);
	if (rc < 0) {
		(void)tool_result_printf(result,
					 "{\"error\":\"MCP server '%s' not "
					 "connected\",\"code\":%d}",
					 binding->client->config.name, rc);
		return rc;
	}

	return mcp_get_prompt(binding->client, NULL, binding->mcp_prompt_name,
			      args_json, result);
}

int mcp_register_server_prompts(struct mcp_client *client,
				struct tool_registry *reg)
{
	if (!client || !reg)
		return -EINVAL;

	int rc = mcp_ensure_connected(client);
	if (rc < 0)
		return rc;
	if (!client->supports_prompts)
		return 0;

	struct arena *arena = arena_create(0);
	if (!arena)
		return -ENOMEM;

	struct mcp_prompt_desc *prompts = NULL;
	int count = 0;
	rc = mcp_list_prompts(client, arena, &prompts, &count);
	if (rc < 0 || count == 0) {
		arena_destroy(arena);
		return rc;
	}

	int registered = 0;
	for (int i = 0; i < count; i++) {
		struct mcp_prompt_binding *binding = calloc(1, sizeof(*binding));
		if (!binding) {
			log_err("mcp: failed to allocate prompt binding");
			continue;
		}
		binding->client = client;
		strncpy(binding->mcp_prompt_name, prompts[i].name, MCP_NAME_MAX - 1);

		char tool_name[TOOL_NAME_MAX];
		snprintf(tool_name, sizeof(tool_name), "mcp_prompt_%s__%s",
			 client->config.name, prompts[i].name);

		for (size_t j = 0; j < strlen(tool_name); j++) {
			char c = tool_name[j];
			if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			      (c >= '0' && c <= '9') || c == '_' || c == '-'))
				tool_name[j] = '_';
		}

		const char *schema = prompts[i].arguments_schema;
		if (!schema || !schema[0])
			schema = "{\"type\":\"object\",\"additionalProperties\":false}";

		rc = tool_register(reg, tool_name, prompts[i].description,
				   schema, mcp_prompt_exec, binding,
				   (tool_user_data_destroy_fn)free);
		if (rc < 0) {
			log_warn("mcp: failed to register prompt '%s' (rc=%d)", tool_name, rc);
			free(binding);
		} else {
			struct tool_entry *e = tool_lookup(reg, tool_name);
			if (e)
				e->flags |= TOOL_FLAG_READONLY;
			log_info("mcp: registered prompt '%s' -> '%s'", prompts[i].name, tool_name);
			registered++;
		}
	}

	arena_destroy(arena);
	return registered;
}
