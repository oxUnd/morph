#include "agent/tool_runtime.h"
#include "mcp/mcp_internal.h"
#include "util/json.h"
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

static void mcp_test_message(char *message, size_t message_cap, const char *fmt, ...)
{
	va_list ap;

	if (!message || message_cap == 0)
		return;
	va_start(ap, fmt);
	vsnprintf(message, message_cap, fmt, ap);
	va_end(ap);
}

int mcp_test_connection(const struct mcp_server_config *cfg, char *message, size_t message_cap)
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
	} else if (cfg->transport == MCP_TRANSPORT_STREAMABLE_HTTP && client &&
		   http_session_status(&client->session) >= 400) {
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
static void mcp_disable_list_capability(struct mcp_client *client, const char *key,
					const char *method)
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
static int mcp_list_all(struct mcp_client *client, const char *method, const char *key,
			struct json **out)
{
	struct json *items = json_array();
	struct json *page = NULL;
	char *params = NULL;
	char *result = NULL;
	morph_strmap_t cursors;
	int rc = morph_strmap_init(&cursors, 16);

	*out = NULL;
	if (rc != 0 || !items) {
		json_free(items);
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
		page = result ? json_parse_prefix(result, NULL, NULL) : NULL;
		free(result);
		result = NULL;
		struct json *list = json_member(page, key);
		struct json *next = json_member(page, "nextCursor");

		if (!json_is_array_value(list) || (next && !json_is_string_value(next))) {
			rc = (MORPH_ERR_PROTOCOL);
			break;
		}
		if (json_count(list) > INT_MAX - json_count(items)) {
			rc = (-EOVERFLOW);
			break;
		}
		while (json_first(list)) {
			struct json *item = json_detach(list, json_first(list));

			if (json_array_add(items, item) != 0) {
				json_free(item);
				rc = (-ENOMEM);
				break;
			}
		}
		if (rc != 0 || !next)
			break;
		if (morph_strmap_contains(&cursors, json_string_value(next))) {
			rc = (MORPH_ERR_PROTOCOL);
			break;
		}
		rc = morph_strmap_set(&cursors, json_string_value(next), client);
		if (rc != 0)
			break;
		struct json *request = json_object();

		if (request && json_add_string(request, "cursor", json_string_value(next)))
			params = json_stringify(request);
		json_free(request);
		if (!params) {
			rc = (-ENOMEM);
			break;
		}
		json_free(page);
		page = NULL;
	}
	free(result);
	free(params);
	json_free(page);
	morph_strmap_cleanup(&cursors);
	if (rc != 0) {
		json_free(items);
		log_warn(client ? client->config.logger : NULL, "mcp: %s failed: %s", method,
			 morph_strerror(rc));
		return rc;
	}
	*out = items;
	return 0;
}

static const char *mcp_string(struct arena *arena, struct json *item, const char *key)
{
	return arena_strdup(arena, json_str_or(item, key, ""));
}

static const char *mcp_schema(struct arena *arena, struct json *value)
{
	char *json = value ? json_stringify(value) : NULL;
	const char *copy = json ? arena_strdup(arena, json) : NULL;

	free(json);
	return copy;
}

/* Build parameters with JSON escaping and no fixed size ceiling. */
static int mcp_named_request(struct mcp_client *client, const char *method, const char *key,
			     const char *name, const char *args, char **result)
{
	struct json *params = json_object();
	char *json;
	int rc;

	if (!params)
		return -ENOMEM;
	if (!json_add_string(params, key, name)) {
		json_free(params);
		return -ENOMEM;
	}
	if (args) {
		struct json *arguments = json_parse_ex(args, NULL, 1, NULL);

		if (!json_is_object_value(arguments)) {
			json_free(arguments);
			json_free(params);
			return MORPH_ERR_PARSE;
		}
		if (json_object_add(params, "arguments", arguments) != 0) {
			json_free(arguments);
			json_free(params);
			return -ENOMEM;
		}
	}
	json = json_stringify(params);
	json_free(params);
	if (!json)
		return -ENOMEM;
	rc = mcp_request(client, method, json, result);
	free(json);
	return rc;
}

int mcp_list_tools(struct mcp_client *client, struct arena *arena, struct mcp_tool_desc **out_tools,
		   int *out_count)
{
	if (!client || !arena || !out_tools || !out_count)
		return -EINVAL;
	*out_tools = NULL;
	*out_count = 0;
	struct json *items = NULL;
	int rc = mcp_list_all(client, "tools/list", "tools", &items);

	if (rc != 0)
		return rc;
	int count = json_count(items);
	struct mcp_tool_desc *entries = NULL;

	if ((size_t)count > SIZE_MAX / sizeof(*entries)) {
		json_free(items);
		return -EOVERFLOW;
	}
	if (count > 0)
		entries = arena_alloc(arena, (size_t)count * sizeof(*entries));
	if (count > 0 && !entries) {
		json_free(items);
		return -ENOMEM;
	}
	struct json *item;
	int i = 0;

	json_foreach (item, items) {
		struct json *schema = json_member(item, "inputSchema");
		struct json *name = json_member(item, "name");

		if (schema && json_is_string_value(name)) {
			rc = mcp_protocol_cache_tool(client, json_string_value(name), schema);
			if (rc == -EINVAL) {
				log_warn(
					client ? client->config.logger : NULL,
					"mcp: excluding tool '%s': invalid x-mcp-header annotation",
					json_string_value(name));
				rc = 0;
				continue;
			}
			if (rc != 0) {
				json_free(items);
				return rc;
			}
		}
		entries[i].name = mcp_string(arena, item, "name");
		entries[i].title = mcp_string(arena, item, "title");
		entries[i].description = mcp_string(arena, item, "description");
		entries[i].input_schema = schema ? mcp_schema(arena, schema)
						 : arena_strdup(arena, TOOL_EMPTY_INPUT_SCHEMA);
		if (rc != 0 || !entries[i].name || !entries[i].title || !entries[i].description ||
		    !entries[i].input_schema) {
			json_free(items);
			return rc != 0 ? rc : -ENOMEM;
		}
		i++;
	}
	json_free(items);
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
			(void)tool_result_success_json_text(
				out_result,
				strdup("{\"error\":\"empty response\",\"isError\":true}"));
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
	struct json *obj = json_parse_prefix(result, NULL, NULL);
	if (!obj) {
		log_err(out_result && out_result->runtime ? out_result->runtime->http.logger : NULL,
			"mcp: tools/call response parse error");
		(void)tool_result_success_json_text(
			out_result,
			strdup("{\"error\":\"response parse error\",\"isError\":true}"));
		free(result);
		return MORPH_ERR_PARSE;
	}

	struct json *is_err = json_member_ci(obj, "isError");
	int tool_error = (is_err && json_is_bool_value(is_err) && json_is_true(is_err));

	/* Keep every content block and extension field alongside readable text. */
	struct json *content = json_member(obj, "content");
	morph_buf_t combined;
	rc = morph_buf_init(&combined, BUFSIZ);
	struct json *item;

	json_foreach (item, content) {
		struct json *text = json_member(item, "text");

		if (rc != 0 || !json_is_string_value(text))
			continue;
		if (combined.len > 0)
			rc = morph_buf_putc(&combined, '\n');
		if (rc == 0)
			rc = morph_buf_puts(&combined, json_string_value(text));
	}
	if (rc == 0 && combined.len > 0) {
		struct json *blocks = json_object_detach(obj, "content");

		if (json_object_add(obj, "contentBlocks", blocks) != 0) {
			json_free(blocks);
			rc = (-ENOMEM);
		}
		if (rc == 0 && !json_add_string(obj, "content", combined.data))
			rc = (-ENOMEM);
	}
	if (rc == 0 && tool_error && !json_has_member_ci(obj, "error")) {
		if (!json_add_string(obj, "error",
				     combined.len > 0 ? combined.data
						      : "MCP tool returned isError"))
			rc = (-ENOMEM);
	}
	morph_buf_cleanup(&combined);
	if (rc == 0) {
		char *json = json_stringify(obj);

		rc = tool_result_success_json_text(out_result, json);
	}
	if (rc != 0) {
		json_free(obj);
		free(result);
		return rc;
	}
	json_free(obj);
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
	struct json *items = NULL;
	int rc = mcp_list_all(client, "resources/list", "resources", &items);

	if (rc != 0)
		return rc;
	int count = json_count(items);
	struct mcp_resource_desc *entries = NULL;

	if ((size_t)count > SIZE_MAX / sizeof(*entries)) {
		json_free(items);
		return -EOVERFLOW;
	}
	if (count > 0)
		entries = arena_alloc(arena, (size_t)count * sizeof(*entries));
	if (count > 0 && !entries) {
		json_free(items);
		return -ENOMEM;
	}
	struct json *item;
	int i = 0;

	json_foreach (item, items) {
		entries[i].uri = mcp_string(arena, item, "uri");
		entries[i].name = mcp_string(arena, item, "name");
		entries[i].description = mcp_string(arena, item, "description");
		entries[i].mime_type = mcp_string(arena, item, "mimeType");
		if (rc != 0 || !entries[i].uri || !entries[i].name || !entries[i].description ||
		    !entries[i].mime_type) {
			json_free(items);
			return rc != 0 ? rc : -ENOMEM;
		}
		i++;
	}
	json_free(items);
	*out_res = entries;
	*out_count = count;
	return 0;
}

/* ----- MCP resources/read ----- */

int mcp_read_resource(struct mcp_client *client, struct arena *arena, const char *uri,
		      struct tool_result *out_content)
{
	if (!client || !uri || !out_content)
		return -EINVAL;

	char *result = NULL;
	int rc = mcp_named_request(client, "resources/read", "uri", uri, NULL, &result);

	if (rc < 0 || !result) {
		free(result);
		return rc ? rc : MORPH_ERR_PROTOCOL;
	}

	struct json *obj = json_parse_prefix(result, NULL, NULL);
	if (!obj) {
		log_err(out_content && out_content->runtime ? out_content->runtime->http.logger
							    : NULL,
			"mcp: resources/read response parse error");
		free(result);
		return -EINVAL;
	}

	/* Extract text from contents array */
	struct json *contents = json_member_ci(obj, "contents");
	if (contents && json_is_array_value(contents)) {
		morph_buf_t combined;
		rc = morph_buf_init(&combined, BUFSIZ);
		struct json *item;

		json_foreach (item, contents) {
			const char *keys[] = {"text", "blob"};

			for (size_t i = 0; rc == 0 && i < sizeof(keys) / sizeof(keys[0]); i++) {
				struct json *value = json_member(item, keys[i]);

				if (!json_is_string_value(value))
					continue;
				if (combined.len > 0)
					rc = morph_buf_putc(&combined, '\n');
				if (rc == 0)
					rc = morph_buf_puts(&combined, json_string_value(value));
			}
		}
		char *text = rc == 0 ? mcp_strdup_result(arena, morph_buf_cstr(&combined)) : NULL;

		morph_buf_cleanup(&combined);
		if (!text) {
			json_free(obj);
			free(result);
			return rc != 0 ? rc : -ENOMEM;
		}
		(void)tool_result_success_json_text(out_content, text);
	} else {
		(void)tool_result_success_json_text(out_content, mcp_strdup_result(arena, result));
	}

	json_free(obj);
	free(result);
	return 0;
}

/* ----- MCP prompts/list ----- */

static int mcp_prompt_arguments_schema(struct arena *arena, struct json *arguments,
				       const char **destination)
{
	struct json *schema = json_object();
	struct json *properties;
	struct json *required;
	struct json *argument;
	char *encoded = NULL;
	int rc = 0;

	if (!schema)
		return -ENOMEM;
	properties = json_add_object(schema, "properties");
	required = json_add_array(schema, "required");
	if (!properties || !required || !json_add_string(schema, "type", "object") ||
	    !json_add_bool(schema, "additionalProperties", 0))
		goto out_free_schema_oom;
	json_foreach (argument, arguments) {
		struct json *name = json_member_ci(argument, "name");
		struct json *description = json_member_ci(argument, "description");
		struct json *mandatory = json_member_ci(argument, "required");
		struct json *property;

		if (!json_is_string_value(name))
			continue;
		property = json_add_object(properties, json_string_value(name));
		if (!property || !json_add_string(property, "type", "string"))
			goto out_free_schema_oom;
		if (json_is_string_value(description) &&
		    !json_add_string(property, "description", json_string_value(description)))
			goto out_free_schema_oom;
		if (json_is_true(mandatory)) {
			struct json *item = json_string(json_string_value(name));

			if (!item)
				goto out_free_schema_oom;
			if (json_array_add(required, item) != 0) {
				json_free(item);
				goto out_free_schema_oom;
			}
		}
	}
	encoded = json_stringify(schema);
	if (!encoded)
		goto out_free_schema_oom;
	*destination = arena_strdup(arena, encoded);
	if (!*destination)
		rc = (-ENOMEM);
	free(encoded);
	json_free(schema);
	return rc;

out_free_schema_oom:
	json_free(schema);
	return -ENOMEM;
}

int mcp_list_prompts(struct mcp_client *client, struct arena *arena,
		     struct mcp_prompt_desc **out_prompts, int *out_count)
{
	if (!client || !arena || !out_prompts || !out_count)
		return -EINVAL;
	*out_prompts = NULL;
	*out_count = 0;
	struct json *items = NULL;
	int rc = mcp_list_all(client, "prompts/list", "prompts", &items);

	if (rc != 0)
		return rc;
	int count = json_count(items);
	struct mcp_prompt_desc *entries = NULL;

	if ((size_t)count > SIZE_MAX / sizeof(*entries)) {
		json_free(items);
		return -EOVERFLOW;
	}
	if (count > 0)
		entries = arena_alloc(arena, (size_t)count * sizeof(*entries));
	if (count > 0 && !entries) {
		json_free(items);
		return -ENOMEM;
	}
	struct json *item;
	int i = 0;

	json_foreach (item, items) {
		entries[i].name = mcp_string(arena, item, "name");
		entries[i].description = mcp_string(arena, item, "description");
		rc = mcp_prompt_arguments_schema(arena, json_member(item, "arguments"),
						 &entries[i].arguments_schema);
		if (rc != 0 || !entries[i].name || !entries[i].description ||
		    !entries[i].arguments_schema) {
			json_free(items);
			return rc != 0 ? rc : -ENOMEM;
		}
		i++;
	}
	json_free(items);
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

	return mcp_call_tool(binding->client, NULL, binding->mcp_tool_name, args_json, result);
}

int mcp_register_server_tools(struct mcp_client *client, struct tool_registry *reg)
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
		struct mcp_tool_binding *binding =
			calloc(1, sizeof(*binding) + strlen(tools[i].name) + 1);
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

int mcp_register_server_resources(struct mcp_client *client, struct tool_registry *reg)
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
		struct mcp_resource_binding *binding =
			calloc(1, sizeof(*binding) + strlen(res[i].uri) + 1);
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

	return mcp_get_prompt(binding->client, NULL, binding->mcp_prompt_name, args_json, result);
}

int mcp_register_server_prompts(struct mcp_client *client, struct tool_registry *reg)
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
		struct mcp_prompt_binding *binding =
			calloc(1, sizeof(*binding) + strlen(prompts[i].name) + 1);
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
