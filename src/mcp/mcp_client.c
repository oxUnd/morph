#include "agent/tool_runtime.h"
#include "mcp/mcp_internal.h"
#include "cJSON.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/array.h"
#include "util/log.h"
#include "util/error.h"
#include "util/name.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *mcp_strdup_result(struct arena *arena, const char *s)
{
	if (!s)
		return NULL;
	/* tool_result_success_json_text takes ownership and frees its input. */
	(void)arena;
	return strdup(s);
}

/* ----- Registry ----- */

void mcp_registry_init(struct morph_log *logger, struct mcp_registry *reg)
{
	if (!reg)
		return;
	memset(reg, 0, sizeof(*reg));
	reg->logger = logger;
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
	client->config.logger = reg->logger;
	client->connected = 0;
	client->server_pid = -1;
	client->stdin_fd = -1;
	client->stdout_fd = -1;
	client->next_req_id = 1;
	pthread_mutex_init(&client->lock, NULL);

	reg->servers[reg->count] = client;
	(void)morph_strmap_set(&reg->by_name, client->config.name, client);
	reg->count++;

	log_info(reg ? reg->logger : NULL, "mcp: registered server '%s' (transport=%s)", cfg->name,
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
	return mcp_protocol_initialize(client);
}

void mcp_disconnect(struct mcp_client *client)
{
	if (!client)
		return;
	if (client->config.transport == MCP_TRANSPORT_STDIO)
		mcp_stdio_disconnect(client);
	else
		mcp_http_disconnect(client);
	mcp_protocol_reset(client);
}

/* ----- Lazy-connect wrapper ----- */

int mcp_ensure_connected(struct mcp_client *client)
{
	if (!client)
		return -EINVAL;

	pthread_mutex_lock(&client->lock);
	if (client->connected || client->connecting) {
		int status = client->connecting ? -EAGAIN : 0;

		pthread_mutex_unlock(&client->lock);
		return status;
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
	if (rc != 0)
		mcp_disconnect(client);

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
	char *result = NULL;
	int rc = mcp_request(client, "ping", NULL, &result);
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
		return -EINVAL;
	}

	mcp_registry_init(cfg ? cfg->logger : NULL, &reg);
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

/* Some servers advertise a capability but omit its list handler. A correlated
 * MethodNotFound disables only that capability, never the usable ones. */
static void mcp_disable_list_capability(struct mcp_client *client,
	const char *key, const char *method)
{
	pthread_mutex_lock(&client->lock);
	if (strcmp(key, "tools") == 0)
		client->supports_tools = 0;
	else if (strcmp(key, "resources") == 0)
		client->supports_resources = 0;
	else if (strcmp(key, "prompts") == 0)
		client->supports_prompts = 0;
	pthread_mutex_unlock(&client->lock);
	log_warn(client ? client->config.logger : NULL,
		 "mcp: '%s' advertised %s but %s is not implemented; "
		 "disabling this capability",
		 client->config.name, key, method);
}

/* Collect every page, rejecting malformed lists and cyclic cursors. */
static int mcp_list_all(struct mcp_client *client, const char *method,
	const char *key, cJSON **out)
{
	cJSON *items = cJSON_CreateArray();
	cJSON *page = NULL;
	char *params = NULL;
	char *result = NULL;
	morph_strmap_t cursors;
	int rc = morph_strmap_init(&cursors, 16);

	*out = NULL;
	if (rc != 0 || !items) {
		cJSON_Delete(items);
		if (rc == 0)
			morph_strmap_cleanup(&cursors);
		return rc != 0 ? rc : -ENOMEM;
	}
	for (;;) {
		int first_page = params == NULL;

		rc = mcp_request(client, method, params, &result);
		free(params);
		params = NULL;
		if (rc == -ENOSYS && first_page) {
			mcp_disable_list_capability(client, key, method);
			rc = 0;
			break;
		}
		if (rc != 0)
			break;
		page = result ? cJSON_Parse(result) : NULL;
		free(result);
		result = NULL;
		cJSON *list = cJSON_GetObjectItemCaseSensitive(page, key);
		cJSON *next = cJSON_GetObjectItemCaseSensitive(page, "nextCursor");

		if (!cJSON_IsArray(list) || (next && !cJSON_IsString(next))) {
			rc = (MORPH_ERR_PROTOCOL);
			break;
		}
		if (cJSON_GetArraySize(list) > INT_MAX - cJSON_GetArraySize(items)) {
			rc = (-EOVERFLOW);
			break;
		}
		while (list->child) {
			cJSON *item = cJSON_DetachItemViaPointer(list, list->child);

			if (!cJSON_AddItemToArray(items, item)) {
				cJSON_Delete(item);
				rc = (-ENOMEM);
				break;
			}
		}
		if (rc != 0 || !next)
			break;
		if (morph_strmap_contains(&cursors, next->valuestring)) {
			rc = (MORPH_ERR_PROTOCOL);
			break;
		}
		rc = morph_strmap_set(&cursors, next->valuestring, client);
		if (rc != 0)
			break;
		cJSON *request = cJSON_CreateObject();

		if (request && cJSON_AddStringToObject(request, "cursor", next->valuestring))
			params = cJSON_PrintUnformatted(request);
		cJSON_Delete(request);
		if (!params) {
			rc = (-ENOMEM);
			break;
		}
		cJSON_Delete(page);
		page = NULL;
	}
	free(result);
	free(params);
	cJSON_Delete(page);
	morph_strmap_cleanup(&cursors);
	if (rc != 0) {
		cJSON_Delete(items);
		log_warn(client ? client->config.logger : NULL, "mcp: %s failed: %s", method,
			 morph_strerror(rc));
		return rc;
	}
	*out = items;
	return 0;
}

static const char *mcp_string(struct arena *arena, cJSON *item, const char *key)
{
	cJSON *value = cJSON_GetObjectItemCaseSensitive(item, key);

	return arena_strdup(arena, cJSON_IsString(value) ? value->valuestring : "");
}

static const char *mcp_schema(struct arena *arena, cJSON *value)
{
	char *json = value ? cJSON_PrintUnformatted(value) : NULL;
	const char *copy = json ? arena_strdup(arena, json) : NULL;

	free(json);
	return copy;
}

/* Build parameters with JSON escaping and no fixed size ceiling. */
static int mcp_named_request(struct mcp_client *client, const char *method,
	const char *key, const char *name, const char *args, char **result)
{
	cJSON *params = cJSON_CreateObject();
	char *json;
	int rc;

	if (!params)
		return -ENOMEM;
	if (!cJSON_AddStringToObject(params, key, name)) {
		cJSON_Delete(params);
		return -ENOMEM;
	}
	if (args) {
		cJSON *arguments = cJSON_ParseWithOpts(args, NULL, 1);

		if (!cJSON_IsObject(arguments)) {
			cJSON_Delete(arguments);
			cJSON_Delete(params);
			return MORPH_ERR_PARSE;
		}
		if (!cJSON_AddItemToObject(params, "arguments", arguments)) {
			cJSON_Delete(arguments);
			cJSON_Delete(params);
			return -ENOMEM;
		}
	}
	json = cJSON_PrintUnformatted(params);
	cJSON_Delete(params);
	if (!json)
		return -ENOMEM;
	rc = mcp_request(client, method, json, result);
	free(json);
	return rc;
}

int mcp_list_tools(struct mcp_client *client, struct arena *arena,
	struct mcp_tool_desc **out_tools, int *out_count)
{
	if (!client || !arena || !out_tools || !out_count)
		return -EINVAL;
	*out_tools = NULL;
	*out_count = 0;
	cJSON *items = NULL;
	int rc = mcp_list_all(client, "tools/list", "tools", &items);

	if (rc != 0)
		return rc;
	int count = cJSON_GetArraySize(items);
	struct mcp_tool_desc *entries = NULL;

	if ((size_t)count > SIZE_MAX / sizeof(*entries)) {
		cJSON_Delete(items);
		return -EOVERFLOW;
	}
	if (count > 0)
		entries = arena_alloc(arena, (size_t)count * sizeof(*entries));
	if (count > 0 && !entries) {
		cJSON_Delete(items);
		return -ENOMEM;
	}
	cJSON *item;
	int i = 0;

	cJSON_ArrayForEach(item, items) {
		cJSON *schema = cJSON_GetObjectItemCaseSensitive(item, "inputSchema");
		cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");

		if (schema && cJSON_IsString(name)) {
			rc = mcp_protocol_cache_tool(client, name->valuestring, schema);
			if (rc == -EINVAL) {
				log_warn(
					client ? client->config.logger : NULL,
					"mcp: excluding tool '%s': invalid x-mcp-header annotation",
					name->valuestring);
				rc = 0;
				continue;
			}
			if (rc != 0) {
				cJSON_Delete(items);
				return rc;
			}
		}
		entries[i].name = mcp_string(arena, item, "name");
		entries[i].title = mcp_string(arena, item, "title");
		entries[i].description = mcp_string(arena, item, "description");
		entries[i].input_schema = schema ? mcp_schema(arena, schema) :
			arena_strdup(arena, TOOL_EMPTY_INPUT_SCHEMA);
		if (rc != 0 || !entries[i].name ||
		    !entries[i].title ||
		    !entries[i].description ||
		    !entries[i].input_schema) {
			cJSON_Delete(items);
			return rc != 0 ? rc : -ENOMEM;
		}
		i++;
	}
	cJSON_Delete(items);
	*out_tools = entries;
	*out_count = i;
	return 0;
}

/* ----- MCP tools/call ----- */

int mcp_call_tool(struct mcp_client *client, struct arena *arena, const char *name,
		  const char *args_json, struct tool_result *out_result)
{
	if (!client || !name || !out_result)
		return -EINVAL;

	if (client->protocol && client->protocol->per_request &&
	    client->config.transport == MCP_TRANSPORT_STREAMABLE_HTTP) {
		pthread_mutex_lock(&client->lock);
		int cached = morph_strmap_contains(&client->tool_schemas, name);
		pthread_mutex_unlock(&client->lock);
		if (!cached) {
			struct arena *discovery = arena_create(0);
			struct mcp_tool_desc *tools = NULL;
			int count = 0;

			if (!discovery)
				return -ENOMEM;
			int rc = mcp_list_tools(client, discovery, &tools, &count);

			arena_destroy(discovery);
			if (rc != 0)
				return rc;
		}
	}

	char *result = NULL;
	int rc = mcp_named_request(client, "tools/call", "name", name,
		args_json && args_json[0] ? args_json : "{}", &result);

	if (rc < 0 || !result) {
		if (rc >= 0 && !result) {
			(void)tool_result_success_json_text(out_result, strdup("{\"error\":\"empty response\",\"isError\":true}"));
			return MORPH_ERR_PROTOCOL;
		}
		(void)tool_result_successf(out_result,
					 "{\"error\":\"MCP call failed\","
					 "\"code\":%d,\"isError\":true}",
					 rc);
		free(result);
		return rc;
	}

	(void)arena;
	/* Parse the tools/call response without discarding structured content. */
	cJSON *obj = cJSON_Parse(result);
	if (!obj) {
		log_err(out_result && out_result->runtime ? out_result->runtime->http.logger : NULL,
			"mcp: tools/call response parse error");
		(void)tool_result_success_json_text(out_result, strdup("{\"error\":\"response parse error\",\"isError\":true}"));
		free(result);
		return MORPH_ERR_PARSE;
	}

	cJSON *is_err = cJSON_GetObjectItem(obj, "isError");
	int tool_error = (is_err && cJSON_IsBool(is_err) && cJSON_IsTrue(is_err));

	/* Keep every content block and extension field alongside readable text. */
	cJSON *content = cJSON_GetObjectItemCaseSensitive(obj, "content");
	morph_buf_t combined;
	rc = morph_buf_init(&combined, BUFSIZ);
	cJSON *item;

	cJSON_ArrayForEach(item, content) {
		cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");

		if (rc != 0 || !cJSON_IsString(text))
			continue;
		if (combined.len > 0)
			rc = morph_buf_putc(&combined, '\n');
		if (rc == 0)
			rc = morph_buf_puts(&combined, text->valuestring);
	}
	if (rc == 0 && combined.len > 0) {
		cJSON *blocks = cJSON_DetachItemFromObjectCaseSensitive(obj, "content");

		if (!cJSON_AddItemToObject(obj, "contentBlocks", blocks)) {
			cJSON_Delete(blocks);
			rc = (-ENOMEM);
		}
		if (rc == 0 && !cJSON_AddStringToObject(obj, "content", combined.data))
			rc = (-ENOMEM);
	}
	if (rc == 0 && tool_error && !cJSON_HasObjectItem(obj, "error")) {
		if (!cJSON_AddStringToObject(obj, "error", combined.len > 0 ?
			combined.data : "MCP tool returned isError"))
			rc = (-ENOMEM);
	}
	morph_buf_cleanup(&combined);
	if (rc == 0) {
		char *json = cJSON_PrintUnformatted(obj);

		rc = tool_result_success_json_text(out_result, json);
	}
	if (rc != 0) {
		cJSON_Delete(obj);
		free(result);
		return rc;
	}
	cJSON_Delete(obj);
	free(result);
	return tool_error ? -EIO : 0;
}

/* ----- MCP resources/list ----- */

int mcp_list_resources(struct mcp_client *client, struct arena *arena,
	struct mcp_resource_desc **out_res, int *out_count)
{
	if (!client || !arena || !out_res || !out_count)
		return -EINVAL;
	*out_res = NULL;
	*out_count = 0;
	cJSON *items = NULL;
	int rc = mcp_list_all(client, "resources/list", "resources", &items);

	if (rc != 0)
		return rc;
	int count = cJSON_GetArraySize(items);
	struct mcp_resource_desc *entries = NULL;

	if ((size_t)count > SIZE_MAX / sizeof(*entries)) {
		cJSON_Delete(items);
		return -EOVERFLOW;
	}
	if (count > 0)
		entries = arena_alloc(arena, (size_t)count * sizeof(*entries));
	if (count > 0 && !entries) {
		cJSON_Delete(items);
		return -ENOMEM;
	}
	cJSON *item;
	int i = 0;

	cJSON_ArrayForEach(item, items) {
		entries[i].uri = mcp_string(arena, item, "uri");
		entries[i].name = mcp_string(arena, item, "name");
		entries[i].description = mcp_string(arena, item, "description");
		entries[i].mime_type = mcp_string(arena, item, "mimeType");
		if (rc != 0 || !entries[i].uri ||
		    !entries[i].name ||
		    !entries[i].description ||
		    !entries[i].mime_type) {
			cJSON_Delete(items);
			return rc != 0 ? rc : -ENOMEM;
		}
		i++;
	}
	cJSON_Delete(items);
	*out_res = entries;
	*out_count = count;
	return 0;
}

/* ----- MCP resources/read ----- */

int mcp_read_resource(struct mcp_client *client, struct arena *arena,
		      const char *uri, struct tool_result *out_content)
{
	if (!client || !uri || !out_content)
		return -EINVAL;

	char *result = NULL;
	int rc = mcp_named_request(client, "resources/read", "uri", uri,
		NULL, &result);

	if (rc < 0 || !result) {
		free(result);
		return rc ? rc : MORPH_ERR_PROTOCOL;
	}

	cJSON *obj = cJSON_Parse(result);
	if (!obj) {
		log_err(out_content && out_content->runtime ? out_content->runtime->http.logger
							    : NULL,
			"mcp: resources/read response parse error");
		free(result);
		return -EINVAL;
	}

	/* Extract text from contents array */
	cJSON *contents = cJSON_GetObjectItem(obj, "contents");
	if (contents && cJSON_IsArray(contents)) {
		morph_buf_t combined;
		rc = morph_buf_init(&combined, BUFSIZ);
		cJSON *item;

		cJSON_ArrayForEach(item, contents) {
			const char *keys[] = {"text", "blob"};

			for (size_t i = 0; rc == 0 && i < sizeof(keys) / sizeof(keys[0]); i++) {
				cJSON *value = cJSON_GetObjectItemCaseSensitive(item, keys[i]);

				if (!cJSON_IsString(value))
					continue;
				if (combined.len > 0)
					rc = morph_buf_putc(&combined, '\n');
				if (rc == 0)
					rc = morph_buf_puts(&combined, value->valuestring);
			}
		}
		char *text = rc == 0 ? mcp_strdup_result(arena, morph_buf_cstr(&combined)) : NULL;

		morph_buf_cleanup(&combined);
		if (!text) {
			cJSON_Delete(obj);
			free(result);
			return rc != 0 ? rc : -ENOMEM;
		}
		(void)tool_result_success_json_text(out_content, text);
	} else {
		(void)tool_result_success_json_text(out_content, mcp_strdup_result(arena, result));
	}

	cJSON_Delete(obj);
	free(result);
	return 0;
}

/* ----- MCP prompts/list ----- */

static int mcp_prompt_arguments_schema(struct arena *arena, cJSON *arguments,
	const char **destination)
{
	cJSON *schema = cJSON_CreateObject();
	cJSON *properties;
	cJSON *required;
	cJSON *argument;
	char *encoded = NULL;
	int rc = 0;

	if (!schema)
		return -ENOMEM;
	properties = cJSON_AddObjectToObject(schema, "properties");
	required = cJSON_AddArrayToObject(schema, "required");
	if (!properties || !required ||
	    !cJSON_AddStringToObject(schema, "type", "object") ||
	    !cJSON_AddBoolToObject(schema, "additionalProperties", 0))
		goto out_free_schema_oom;
	cJSON_ArrayForEach(argument, arguments) {
		cJSON *name = cJSON_GetObjectItem(argument, "name");
		cJSON *description = cJSON_GetObjectItem(argument, "description");
		cJSON *mandatory = cJSON_GetObjectItem(argument, "required");
		cJSON *property;

		if (!cJSON_IsString(name))
			continue;
		property = cJSON_AddObjectToObject(properties, name->valuestring);
		if (!property || !cJSON_AddStringToObject(property, "type", "string"))
			goto out_free_schema_oom;
		if (cJSON_IsString(description) &&
		    !cJSON_AddStringToObject(property, "description", description->valuestring))
			goto out_free_schema_oom;
		if (cJSON_IsTrue(mandatory)) {
			cJSON *item = cJSON_CreateString(name->valuestring);

			if (!item)
				goto out_free_schema_oom;
			if (!cJSON_AddItemToArray(required, item)) {
				cJSON_Delete(item);
				goto out_free_schema_oom;
			}
		}
	}
	encoded = cJSON_PrintUnformatted(schema);
	if (!encoded)
		goto out_free_schema_oom;
	*destination = arena_strdup(arena, encoded);
	if (!*destination)
		rc = (-ENOMEM);
	free(encoded);
	cJSON_Delete(schema);
	return rc;

out_free_schema_oom:
	cJSON_Delete(schema);
	return -ENOMEM;
}

int mcp_list_prompts(struct mcp_client *client, struct arena *arena,
	struct mcp_prompt_desc **out_prompts, int *out_count)
{
	if (!client || !arena || !out_prompts || !out_count)
		return -EINVAL;
	*out_prompts = NULL;
	*out_count = 0;
	cJSON *items = NULL;
	int rc = mcp_list_all(client, "prompts/list", "prompts", &items);

	if (rc != 0)
		return rc;
	int count = cJSON_GetArraySize(items);
	struct mcp_prompt_desc *entries = NULL;

	if ((size_t)count > SIZE_MAX / sizeof(*entries)) {
		cJSON_Delete(items);
		return -EOVERFLOW;
	}
	if (count > 0)
		entries = arena_alloc(arena, (size_t)count * sizeof(*entries));
	if (count > 0 && !entries) {
		cJSON_Delete(items);
		return -ENOMEM;
	}
	cJSON *item;
	int i = 0;

	cJSON_ArrayForEach(item, items) {
		entries[i].name = mcp_string(arena, item, "name");
		entries[i].description = mcp_string(arena, item, "description");
		rc = mcp_prompt_arguments_schema(arena,
			cJSON_GetObjectItemCaseSensitive(item, "arguments"),
			&entries[i].arguments_schema);
		if (rc != 0 || !entries[i].name ||
		    !entries[i].description ||
		    !entries[i].arguments_schema) {
			cJSON_Delete(items);
			return rc != 0 ? rc : -ENOMEM;
		}
		i++;
	}
	cJSON_Delete(items);
	*out_prompts = entries;
	*out_count = count;
	return 0;
}

/* ----- MCP prompts/get ----- */

int mcp_get_prompt(struct mcp_client *client, struct arena *arena, const char *name,
		   const char *args_json, struct tool_result *out_result)
{
	if (!client || !name || !out_result)
		return -EINVAL;

	char *result = NULL;
	int rc = mcp_named_request(client, "prompts/get", "name", name,
		args_json && args_json[0] ? args_json : "{}", &result);

	if (rc < 0 || !result) {
		free(result);
		return rc ? rc : MORPH_ERR_PROTOCOL;
	}

	(void)tool_result_success_json_text(out_result, mcp_strdup_result(arena, result));
	free(result);
	return 0;
}

/* ----- morph tool registry integration ----- */

static void sanitize_tool_name(char *name)
{
	for (char *p = name; *p; p++) {
		if (!morph_name_char_valid(*p, "_-"))
			*p = '_';
	}
}

struct mcp_tool_binding {
	struct mcp_client *client;
	char mcp_tool_name[];
};

static int mcp_tool_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct mcp_tool_binding *binding = (struct mcp_tool_binding *)user_data;
	if (!binding || !binding->client)
		return -EINVAL;

	int rc = mcp_ensure_connected(binding->client);
	if (rc < 0) {
		(void)tool_result_successf(result,
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
	int first_error = 0;
	for (int i = 0; i < count; i++) {
		struct mcp_tool_binding *binding = calloc(1,
			sizeof(*binding) + strlen(tools[i].name) + 1);
		if (!binding) {
			log_err(client ? client->config.logger : NULL,
				"mcp: failed to allocate tool binding");
			first_error = -ENOMEM;
			break;
		}
		binding->client = client;
		memcpy(binding->mcp_tool_name, tools[i].name, strlen(tools[i].name) + 1);

		char tool_name[TOOL_NAME_MAX];
		int name_len = snprintf(tool_name, sizeof(tool_name), "mcp_%s__%s",
			 client->config.name, tools[i].name);
		if (name_len < 0 || (size_t)name_len >= sizeof(tool_name)) {
			log_warn(client ? client->config.logger : NULL,
				 "mcp: capability name exceeds registry name limit: %s",
				 tools[i].name);
			free(binding);
			if (first_error == 0)
				first_error = -ENAMETOOLONG;
			continue;
		}

		sanitize_tool_name(tool_name);

		const char *schema = tools[i].input_schema;
		if (!schema || !schema[0])
			schema = TOOL_EMPTY_INPUT_SCHEMA;

		struct tool_spec spec = {
			.origin = TOOL_ORIGIN_MCP,
			.name = tool_name,
			.title = tools[i].title,
			.description = tools[i].description,
			.input_schema = schema,
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.exec = mcp_tool_exec,
			.user_data = binding,
			.user_data_destroy = (tool_user_data_destroy_fn)free,
		};
		struct tool_entry *existing = tool_lookup(reg, spec.name);
		if (existing && existing->exec == spec.exec && existing->user_data) {
			struct mcp_tool_binding *previous = existing->user_data;

			if (previous->client == client &&
			    strcmp(previous->mcp_tool_name, binding->mcp_tool_name) == 0) {
				free(binding);
				registered++;
				continue;
			}
		}

		rc = tool_register(reg, &spec);
		if (rc < 0) {
			log_warn(client ? client->config.logger : NULL,
				 "mcp: failed to register tool '%s': %s", tool_name,
				 morph_strerror(rc));
			free(binding);
			if (first_error == 0)
				first_error = rc;
		} else {
			log_info(client ? client->config.logger : NULL,
				 "mcp: registered tool '%s' -> '%s'", tools[i].name, tool_name);
			registered++;
		}
	}

	arena_destroy(arena);
	return first_error != 0 ? first_error : registered;
}

struct mcp_resource_binding {
	struct mcp_client *client;
	char uri[];
};

static int mcp_resource_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct mcp_resource_binding *binding = (struct mcp_resource_binding *)user_data;
	if (!binding || !binding->client)
		return -EINVAL;

	int rc = mcp_ensure_connected(binding->client);
	if (rc < 0) {
		(void)tool_result_successf(result,
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
	int first_error = 0;
	for (int i = 0; i < count; i++) {
		struct mcp_resource_binding *binding = calloc(1,
			sizeof(*binding) + strlen(res[i].uri) + 1);
		if (!binding) {
			log_err(client ? client->config.logger : NULL,
				"mcp: failed to allocate resource binding");
			first_error = -ENOMEM;
			break;
		}
		binding->client = client;
		memcpy(binding->uri, res[i].uri, strlen(res[i].uri) + 1);

		char tool_name[TOOL_NAME_MAX];
		int name_len = snprintf(tool_name, sizeof(tool_name), "mcp_res_%s__%s",
			 client->config.name, res[i].name);
		if (name_len < 0 || (size_t)name_len >= sizeof(tool_name)) {
			log_warn(client ? client->config.logger : NULL,
				 "mcp: capability name exceeds registry name limit: %s",
				 res[i].name);
			free(binding);
			if (first_error == 0)
				first_error = -ENAMETOOLONG;
			continue;
		}

		sanitize_tool_name(tool_name);

		morph_buf_t description;
		rc = morph_buf_init_arena(&description, arena, 128);
		if (rc == 0)
			rc = morph_buf_printf(&description, "Read MCP resource: %s (%s)",
				res[i].name, res[i].uri);
		if (rc != 0) {
			free(binding);
			first_error = rc;
			break;
		}

		struct tool_spec spec = {
			.origin = TOOL_ORIGIN_MCP,
			.name = tool_name,
			.description = morph_buf_cstr(&description),
			.input_schema = TOOL_EMPTY_INPUT_SCHEMA,
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.exec = mcp_resource_exec,
			.user_data = binding,
			.user_data_destroy = (tool_user_data_destroy_fn)free,
			.flags = TOOL_FLAG_READONLY,
		};
		struct tool_entry *existing = tool_lookup(reg, spec.name);
		if (existing && existing->exec == spec.exec && existing->user_data) {
			struct mcp_resource_binding *previous = existing->user_data;

			if (previous->client == client &&
			    strcmp(previous->uri, binding->uri) == 0) {
				free(binding);
				registered++;
				continue;
			}
		}

		rc = tool_register(reg, &spec);
		if (rc < 0) {
			log_warn(client ? client->config.logger : NULL,
				 "mcp: failed to register resource '%s': %s", tool_name,
				 morph_strerror(rc));
			free(binding);
			if (first_error == 0)
				first_error = rc;
		} else {
			struct tool_entry *e = tool_lookup(reg, tool_name);
			if (e)
				e->flags |= TOOL_FLAG_READONLY;
			log_info(client ? client->config.logger : NULL,
				 "mcp: registered resource '%s' -> '%s'", res[i].name, tool_name);
			registered++;
		}
	}

	arena_destroy(arena);
	return first_error != 0 ? first_error : registered;
}

struct mcp_prompt_binding {
	struct mcp_client *client;
	char mcp_prompt_name[];
};

static int mcp_prompt_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct mcp_prompt_binding *binding = (struct mcp_prompt_binding *)user_data;
	if (!binding || !binding->client)
		return -EINVAL;

	int rc = mcp_ensure_connected(binding->client);
	if (rc < 0) {
		(void)tool_result_successf(result,
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
	int first_error = 0;
	for (int i = 0; i < count; i++) {
		struct mcp_prompt_binding *binding = calloc(1,
			sizeof(*binding) + strlen(prompts[i].name) + 1);
		if (!binding) {
			log_err(client ? client->config.logger : NULL,
				"mcp: failed to allocate prompt binding");
			first_error = -ENOMEM;
			break;
		}
		binding->client = client;
		memcpy(binding->mcp_prompt_name, prompts[i].name, strlen(prompts[i].name) + 1);

		char tool_name[TOOL_NAME_MAX];
		int name_len = snprintf(tool_name, sizeof(tool_name), "mcp_prompt_%s__%s",
			 client->config.name, prompts[i].name);
		if (name_len < 0 || (size_t)name_len >= sizeof(tool_name)) {
			log_warn(client ? client->config.logger : NULL,
				 "mcp: capability name exceeds registry name limit: %s",
				 prompts[i].name);
			free(binding);
			if (first_error == 0)
				first_error = -ENAMETOOLONG;
			continue;
		}

		sanitize_tool_name(tool_name);

		const char *schema = prompts[i].arguments_schema;
		if (!schema || !schema[0])
			schema = TOOL_EMPTY_INPUT_SCHEMA;

		struct tool_spec spec = {
			.origin = TOOL_ORIGIN_MCP,
			.name = tool_name,
			.description = prompts[i].description,
			.input_schema = schema,
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.exec = mcp_prompt_exec,
			.user_data = binding,
			.user_data_destroy = (tool_user_data_destroy_fn)free,
		};
		struct tool_entry *existing = tool_lookup(reg, spec.name);
		if (existing && existing->exec == spec.exec && existing->user_data) {
			struct mcp_prompt_binding *previous = existing->user_data;

			if (previous->client == client &&
			    strcmp(previous->mcp_prompt_name, binding->mcp_prompt_name) == 0) {
				free(binding);
				registered++;
				continue;
			}
		}

		rc = tool_register(reg, &spec);
		if (rc < 0) {
			log_warn(client ? client->config.logger : NULL,
				 "mcp: failed to register prompt '%s': %s", tool_name,
				 morph_strerror(rc));
			free(binding);
			if (first_error == 0)
				first_error = rc;
		} else {
			struct tool_entry *e = tool_lookup(reg, tool_name);
			if (e)
				e->flags |= TOOL_FLAG_READONLY;
			log_info(client ? client->config.logger : NULL,
				 "mcp: registered prompt '%s' -> '%s'", prompts[i].name, tool_name);
			registered++;
		}
	}

	arena_destroy(arena);
	return first_error != 0 ? first_error : registered;
}
