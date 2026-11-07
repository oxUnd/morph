#include "mcp_internal.h"
#include "util/error.h"
#include "util/log.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MCP_REQUEST_TIMEOUT_MS 30000
#define MCP_PROBE_TIMEOUT_MS 5000
#define MCP_META_PREFIX "io.modelcontextprotocol/"

/* Versions are explicit: an unknown date never silently selects a codec. */
static const struct mcp_protocol_adapter adapters[] = {
	{MCP_PROTOCOL_VERSION, 1},
	{MCP_LEGACY_PROTOCOL_VERSION, 0},
	{"2025-06-18", 0},
	{"2025-03-26", 0},
	{"2024-11-05", 0},
};

const struct mcp_protocol_adapter *mcp_protocol_find(const char *version)
{
	if (!version)
		return NULL;
	for (size_t i = 0; i < sizeof(adapters) / sizeof(adapters[0]); i++) {
		if (strcmp(version, adapters[i].version) == 0)
			return &adapters[i];
	}
	return NULL;
}

static int modern_metadata(struct json *params)
{
	struct json *meta = json_member(params, "_meta");

	if (!meta)
		meta = json_add_object(params, "_meta");
	if (!json_is_object_value(meta))
		return -EINVAL;
	const char *keys[] = {MCP_META_PREFIX "protocolVersion",
		MCP_META_PREFIX "clientInfo", MCP_META_PREFIX "clientCapabilities"};

	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		while (json_member(meta, keys[i]))
			json_object_remove(meta, keys[i]);
	}
	struct json *info = json_add_object(meta, MCP_META_PREFIX "clientInfo");

	if (!info || !json_add_string(info, "name", "morph") ||
	    !json_add_string(info, "version", MORPH_VERSION) ||
	    !json_add_string(meta, MCP_META_PREFIX "protocolVersion", MCP_PROTOCOL_VERSION) ||
	    !json_add_object(meta, MCP_META_PREFIX "clientCapabilities"))
		return -ENOMEM;
	return 0;
}

static int protocol_headers(struct mcp_client *client, const struct mcp_protocol_adapter *adapter,
			    const char *method, struct json *params, struct arena *arena,
			    morph_array_t *headers)
{
	/* The legacy version is not negotiated until initialize has completed. */
	if (!adapter->per_request && strcmp(method, "initialize") == 0)
		return 0;
	int rc = mcp_protocol_add_header(arena, headers,
		"MCP-Protocol-Version", adapter->version);

	if (rc != 0)
		return rc;
	if (!adapter->per_request) {
		if (client->session_id)
			return mcp_protocol_add_header(arena, headers,
				"Mcp-Session-Id", client->session_id);
		return 0;
	}
	rc = mcp_protocol_add_header(arena, headers, "Mcp-Method", method);
	if (rc != 0)
		return rc;
	const char *key = NULL;

	if (strcmp(method, "tools/call") == 0 || strcmp(method, "prompts/get") == 0)
		key = "name";
	else if (strcmp(method, "resources/read") == 0)
		key = "uri";
	if (key) {
		struct json *name = json_member(params, key);

		if (!json_is_string_value(name))
			return MORPH_TRACE_ERROR(client ? client->config.logger : NULL, -EINVAL);
		rc = mcp_protocol_add_header(arena, headers, "Mcp-Name", json_string_value(name));
		if (rc != 0)
			return rc;
	}
	if (strcmp(method, "tools/call") == 0)
		return mcp_protocol_tool_headers(client, params, arena, headers);
	return 0;
}

int mcp_protocol_encode(struct mcp_client *client,
	const struct mcp_protocol_adapter *adapter, int id, const char *method,
	const char *params_json, struct arena *arena, struct mcp_wire_request *out)
{
	if (!client || !adapter || !method || !arena || !out)
		return MORPH_TRACE_ERROR(client ? client->config.logger : NULL, -EINVAL);
	memset(out, 0, sizeof(*out));
	out->id = id;
	int rc = morph_array_init(&out->headers, 8, sizeof(out->json));

	if (rc != 0)
		return rc;
	struct json *params = params_json && params_json[0]
				      ? json_parse_ex(params_json, NULL, 1, NULL)
				      : json_object();

	if (!json_is_object_value(params)) {
		json_free(params);
		return MORPH_TRACE_ERROR(client ? client->config.logger : NULL, MORPH_ERR_PARSE);
	}
	if (adapter->per_request)
		rc = modern_metadata(params);
	if (rc == 0 && client->config.transport == MCP_TRANSPORT_STREAMABLE_HTTP)
		rc = protocol_headers(client, adapter, method, params, arena, &out->headers);
	struct json *request = json_object();

	if (rc == 0 && (!request || !json_add_string(request, "jsonrpc", "2.0") ||
			!json_add_string(request, "method", method) ||
			(id >= 0 && !json_add_number(request, "id", id))))
		rc = MORPH_TRACE_ERROR(client ? client->config.logger : NULL, -ENOMEM);
	if (rc == 0 && (adapter->per_request || (params_json && params_json[0]))) {
		if ((json_object_add(request, "params", params) == 0))
			params = NULL;
		else
			rc = MORPH_TRACE_ERROR(client ? client->config.logger : NULL, -ENOMEM);
	}
	if (rc == 0) {
		char *json = json_stringify(request);

		out->json = json ? arena_strdup(arena, json) : NULL;
		free(json);
		if (!out->json)
			rc = MORPH_TRACE_ERROR(client ? client->config.logger : NULL, -ENOMEM);
	}
	json_free(params);
	json_free(request);
	return rc;
}

int mcp_request_timeout_ms(const struct mcp_client *client)
{
	if (client->config.connect_timeout > 0 &&
	    (client->connecting || client->interrupt_check))
		return client->config.connect_timeout > INT_MAX / 1000 ?
			INT_MAX : client->config.connect_timeout * 1000;
	return MCP_REQUEST_TIMEOUT_MS;
}

void mcp_wire_reply_cleanup(struct mcp_wire_reply *reply)
{
	free(reply->json);
	free(reply->session_id);
	memset(reply, 0, sizeof(*reply));
}

/* Caller holds client->lock for the whole exchange, including session updates. */
static int protocol_exchange(struct mcp_client *client,
	const struct mcp_protocol_adapter *adapter, int id, const char *method,
	const char *params, int timeout_ms, struct mcp_wire_reply *reply)
{
	struct arena *arena = arena_create(0);
	struct mcp_wire_request request = {0};
	int rc;

	if (!arena)
		return MORPH_TRACE_ERROR(client ? client->config.logger : NULL, -ENOMEM);
	rc = mcp_protocol_encode(client, adapter, id, method, params, arena, &request);
	if (rc == 0) {
		if (client->config.transport == MCP_TRANSPORT_STDIO)
			rc = mcp_stdio_exchange(client, &request, timeout_ms, reply);
		else
			rc = mcp_http_exchange(client, &request, timeout_ms, reply);
	}
	morph_array_cleanup(&request.headers);
	arena_destroy(arena);
	return rc;
}

int mcp_protocol_decode(struct morph_log *logger, const struct mcp_wire_reply *reply, int id,
			struct json **out)
{
	*out = NULL;
	if (reply->http_status == 401 || reply->http_status == 403)
		return MORPH_TRACE_ERROR(logger, -EACCES);
	if (reply->http_status >= 500)
		return MORPH_TRACE_ERROR(logger, MORPH_ERR_API);
	struct json *object = reply->json ? json_parse_ex(reply->json, NULL, 1, NULL) : NULL;

	if (!object)
		return MORPH_TRACE_ERROR(logger, MORPH_ERR_PARSE);
	struct json *version = json_member(object, "jsonrpc");
	struct json *response_id = json_member(object, "id");
	struct json *error = json_member(object, "error");
	struct json *result = json_member(object, "result");
	int matches = json_is_number_value(response_id) && json_number_value(response_id) == id;

	/* HTTP header validation can fail before the server reads the request ID. */
	if (!matches && reply->http_status == 400 && !response_id && error)
		matches = 1;
	struct json *code = json_member(error, "code");
	struct json *message = json_member(error, "message");
	int valid_error = !error || (json_is_object_value(error) && json_is_number_value(code) &&
				     json_number_value(code) >= INT_MIN &&
				     json_number_value(code) <= INT_MAX &&
				     (double)json_int_value(code) == json_number_value(code) &&
				     json_is_string_value(message));

	if (!json_is_object_value(object) || !json_is_string_value(version) ||
	    strcmp(json_string_value(version), "2.0") != 0 || !matches || !valid_error ||
	    (!!error == !!result) || (result && !json_is_object_value(result))) {
		json_free(object);
		return MORPH_TRACE_ERROR(logger, MORPH_ERR_PROTOCOL);
	}
	*out = object;
	if (error || reply->http_status >= 400)
		return MORPH_TRACE_ERROR(logger, MORPH_ERR_PROTOCOL);
	return 0;
}

static int result_type(struct morph_log *logger, const struct mcp_protocol_adapter *adapter,
		       struct json *result)
{
	if (!adapter->per_request)
		return 0;
	struct json *type = json_member(result, "resultType");

	if (!type ||
	    (json_is_string_value(type) && strcmp(json_string_value(type), "complete") == 0))
		return 0;
	if (json_is_string_value(type) && strcmp(json_string_value(type), "input_required") == 0) {
		log_warn(logger,
			 "mcp: server requested input without a supported client capability");
		return MORPH_TRACE_ERROR(logger, -ENOTSUP);
	}
	return MORPH_TRACE_ERROR(logger, MORPH_ERR_PROTOCOL);
}

static int request_locked(struct mcp_client *client, int id, const char *method,
	const char *params, char **out)
{
	const struct mcp_protocol_adapter *adapter = client->protocol;

	if (!adapter)
		return MORPH_TRACE_ERROR(client ? client->config.logger : NULL,
			MORPH_ERR_NOT_INITIALIZED);
	struct mcp_wire_reply reply = {0};
	struct json *object = NULL;
	int rc = protocol_exchange(client, adapter, id, method, params,
		mcp_request_timeout_ms(client), &reply);

	if (rc == 0)
		rc = mcp_protocol_decode(client->config.logger, &reply, id, &object);
	if (rc == 0) {
		struct json *result = json_member(object, "result");

		rc = result_type(client ? client->config.logger : NULL, adapter, result);
		if (rc == 0 && out) {
			*out = json_stringify(result);
			if (!*out)
				rc = MORPH_TRACE_ERROR(client ? client->config.logger : NULL,
					-ENOMEM);
		}
	} else if (object) {
		struct json *error = json_member(object, "error");
		struct json *code = json_member(error, "code");
		struct json *message = json_member(error, "message");

		/* Only the validated remote error denotes an absent RPC method. */
		if (rc == MORPH_ERR_PROTOCOL && json_is_number_value(code)) {
			log_warn(client ? client->config.logger : NULL,
				 "mcp: %s on '%s': JSON-RPC %d: %s", method, client->config.name,
				 json_int_value(code), json_string_value(message));
			if (json_int_value(code) == -32601)
				rc = MORPH_TRACE_ERROR(client ? client->config.logger : NULL,
					-ENOSYS);
		}
		if (out)
			*out = json_stringify(object);
	}
	if (rc != 0)
		log_warn(client ? client->config.logger : NULL, "mcp: %s on '%s' failed: %s",
			 method, client->config.name, morph_strerror(rc));
	json_free(object);
	mcp_wire_reply_cleanup(&reply);
	return rc;
}

int mcp_request(struct mcp_client *client, const char *method,
	const char *params, char **result)
{
	if (!client || !method)
		return MORPH_TRACE_ERROR(client ? client->config.logger : NULL, -EINVAL);
	if (result)
		*result = NULL;
	pthread_mutex_lock(&client->lock);
	int rc;

	if (client->next_req_id >= INT_MAX)
		rc = -EOVERFLOW;
	else
		rc = request_locked(client, client->next_req_id++, method, params, result);
	pthread_mutex_unlock(&client->lock);
	return rc;
}

static int version_list_contains(struct json *versions, const char *version)
{
	struct json *item;

	json_foreach (item, versions) {
		if (json_is_string_value(item) && strcmp(json_string_value(item), version) == 0)
			return 1;
	}
	return 0;
}

static void server_details(struct mcp_client *client, struct json *result, int modern)
{
	struct json *caps = json_member(result, "capabilities");
	struct json *info;

	if (modern) {
		struct json *meta = json_member(result, "_meta");

		info = json_member(meta, MCP_META_PREFIX "serverInfo");
	} else {
		info = json_member(result, "serverInfo");
	}
	struct json *name = json_member(info, "name");
	struct json *version = json_member(info, "version");

	if (json_is_string_value(name))
		snprintf(client->server_name, sizeof(client->server_name), "%s",
			 json_string_value(name));
	if (json_is_string_value(version))
		snprintf(client->server_version, sizeof(client->server_version), "%s",
			 json_string_value(version));
	client->supports_tools = json_is_object_value(json_member(caps, "tools"));
	client->supports_resources = json_is_object_value(json_member(caps, "resources"));
	client->supports_prompts = json_is_object_value(json_member(caps, "prompts"));
}

/* Only positive legacy evidence permits downgrade; auth/outages never do. */
static int modern_probe(struct mcp_client *client, int *legacy)
{
	const struct mcp_protocol_adapter *modern = mcp_protocol_find(MCP_PROTOCOL_VERSION);
	struct mcp_wire_reply reply = {0};
	struct json *object = NULL;
	int stdio_transport = client->config.transport == MCP_TRANSPORT_STDIO;
	int timeout = mcp_request_timeout_ms(client);
	int automatic = !client->config.protocol_version[0] ||
		strcmp(client->config.protocol_version, "auto") == 0;

	if (stdio_transport && automatic) {
		/* Leave room for a legacy handshake inside the overall startup deadline. */
		if (client->config.connect_timeout > 0)
			timeout /= 4;
		if (timeout > MCP_PROBE_TIMEOUT_MS)
			timeout = MCP_PROBE_TIMEOUT_MS;
	}
	int id = client->next_req_id++;
	int rc = protocol_exchange(client, modern, id, "server/discover", NULL,
		timeout, &reply);

	*legacy = 0;
	if (rc == 0)
		rc = mcp_protocol_decode(client->config.logger, &reply, id, &object);
	struct json *evidence = reply.json ? json_parse_prefix(reply.json, NULL, NULL) : NULL;
	struct json *error = json_member(evidence, "error");
	struct json *code = json_member(error, "code");
	int modern_error = rc != -EACCES && reply.http_status < 500 && json_is_number_value(code) &&
			   (json_int_value(code) == -32020 || json_int_value(code) == -32021 ||
			    json_int_value(code) == -32022);

	if (modern_error) {
		/* This revision is our only modern codec: never negotiate an unknown date. */
		log_warn(client ? client->config.logger : NULL,
			 "mcp: modern discovery rejected by '%s': %s", client->config.name,
			 reply.json ? reply.json : "missing response");
		rc = MORPH_TRACE_ERROR(client ? client->config.logger : NULL,
				       json_int_value(code) == -32022 ? -EPROTONOSUPPORT
								      : MORPH_ERR_PROTOCOL);
	} else if (rc == 0) {
		struct json *result = json_member(object, "result");
		struct json *versions = json_member(result, "supportedVersions");
		struct json *caps = json_member(result, "capabilities");

		rc = result_type(client ? client->config.logger : NULL, modern, result);
		if (rc == 0 && (!json_is_array_value(versions) || !json_is_object_value(caps)))
			rc = MORPH_TRACE_ERROR(client ? client->config.logger : NULL,
				MORPH_ERR_PROTOCOL);
		if (rc == 0 && !version_list_contains(versions, modern->version))
			rc = MORPH_TRACE_ERROR(client ? client->config.logger : NULL,
				-EPROTONOSUPPORT);
		if (rc == 0) {
			client->protocol = modern;
			server_details(client, result, 1);
		}
	} else if ((stdio_transport && (rc == -ETIMEDOUT || rc == -EPIPE ||
		(rc == MORPH_ERR_PROTOCOL && error))) ||
		(!stdio_transport && ((reply.http_status >= 400 && reply.http_status < 500 &&
		reply.http_status != 401 && reply.http_status != 403) ||
		(rc == MORPH_ERR_PROTOCOL && object && error && reply.http_status < 400)))) {
		*legacy = 1;
		rc = 0;
	}
	json_free(evidence);
	json_free(object);
	mcp_wire_reply_cleanup(&reply);
	return rc;
}

static int legacy_initialize(struct mcp_client *client,
	const struct mcp_protocol_adapter *legacy, int pinned)
{
	morph_buf_t params;
	int rc = morph_buf_init(&params, 128);

	if (rc != 0)
		return rc;
	rc = morph_buf_printf(&params, "{\"protocolVersion\":\"%s\","
		"\"capabilities\":{},\"clientInfo\":{\"name\":\"morph\","
		"\"version\":\"" MORPH_VERSION "\"}}", legacy->version);
	if (rc != 0) {
		morph_buf_cleanup(&params);
		return rc;
	}
	struct mcp_wire_reply reply = {0};
	struct json *object = NULL;
	int id = client->next_req_id++;
	rc = protocol_exchange(client, legacy, id, "initialize", morph_buf_cstr(&params),
		mcp_request_timeout_ms(client), &reply);
	morph_buf_cleanup(&params);

	if (rc == 0)
		rc = mcp_protocol_decode(client->config.logger, &reply, id, &object);
	if (rc == 0) {
		struct json *result = json_member(object, "result");
		struct json *version = json_member(result, "protocolVersion");
		struct json *caps = json_member(result, "capabilities");
		const struct mcp_protocol_adapter *selected =
			json_is_string_value(version)
				? mcp_protocol_find(json_string_value(version))
				: NULL;

		if (!selected || selected->per_request || !json_is_object_value(caps) ||
		    (pinned && selected != legacy) ||
		    (client->config.transport == MCP_TRANSPORT_STREAMABLE_HTTP &&
		     strcmp(selected->version, "2024-11-05") == 0)) {
			rc = MORPH_TRACE_ERROR(client ? client->config.logger : NULL,
				-EPROTONOSUPPORT);
		} else {
			client->protocol = selected;
			server_details(client, result, 0);
			client->session_id = reply.session_id;
			reply.session_id = NULL;
		}
	}
	json_free(object);
	mcp_wire_reply_cleanup(&reply);
	if (rc != 0)
		return rc;
	/* A notification has no ID and no JSON-RPC response. */
	rc = protocol_exchange(client, client->protocol, -1,
		"notifications/initialized", NULL, mcp_request_timeout_ms(client), &reply);
	if (rc == 0 && reply.http_status >= 400)
		rc = MORPH_TRACE_ERROR(client ? client->config.logger : NULL, MORPH_ERR_API);
	mcp_wire_reply_cleanup(&reply);
	return rc;
}

int mcp_protocol_initialize(struct mcp_client *client)
{
	int legacy = 0;
	int rc;

	if (!client)
		return MORPH_TRACE_ERROR(client ? client->config.logger : NULL, -EINVAL);
	const char *version = client->config.protocol_version;
	int pinned = version[0] && strcmp(version, "auto") != 0;
	const struct mcp_protocol_adapter *selected = pinned ? mcp_protocol_find(version) : NULL;

	if (pinned && (!selected ||
	    (client->config.transport == MCP_TRANSPORT_STREAMABLE_HTTP &&
	    strcmp(selected->version, "2024-11-05") == 0)))
		return MORPH_TRACE_ERROR(client ? client->config.logger : NULL, -EPROTONOSUPPORT);
	pthread_mutex_lock(&client->lock);
	mcp_protocol_reset(client);
	client->next_req_id = 1;
	if (pinned && !selected->per_request)
		rc = legacy_initialize(client, selected, 1);
	else
		rc = modern_probe(client, &legacy);
	if (rc == 0 && legacy && pinned)
		rc = MORPH_TRACE_ERROR(client ? client->config.logger : NULL, -EPROTONOSUPPORT);
	if (rc == 0 && client->interrupt_check &&
	    client->interrupt_check(client->interrupt_user_data))
		rc = MORPH_TRACE_ERROR(client ? client->config.logger : NULL, -ECANCELED);
	if (rc == 0 && legacy) {
		if (client->config.transport == MCP_TRANSPORT_STDIO) {
			/* Legacy servers may exit or retain probe state. Start a clean process. */
			mcp_stdio_disconnect(client);
			if (client->interrupt_check &&
			    client->interrupt_check(client->interrupt_user_data))
				rc = -ECANCELED;
			else
				rc = mcp_stdio_connect(client);
			client->next_req_id = 1;
		}
		if (rc == 0)
			rc = legacy_initialize(client,
				mcp_protocol_find(MCP_LEGACY_PROTOCOL_VERSION), 0);
	}
	if (rc == 0) {
		snprintf(client->negotiated_version, sizeof(client->negotiated_version),
			"%s", client->protocol->version);
		client->connected = 1;
		log_info(client ? client->config.logger : NULL, "mcp: '%s' selected protocol %s",
			 client->config.name, client->negotiated_version);
	}
	pthread_mutex_unlock(&client->lock);
	return rc;
}

void mcp_protocol_reset(struct mcp_client *client)
{
	for (size_t i = 0; i < client->tool_schemas.cap; i++)
		json_free(client->tool_schemas.entries[i].value);
	morph_strmap_cleanup(&client->tool_schemas);
	free(client->session_id);
	client->session_id = NULL;
	client->protocol = NULL;
	client->connected = 0;
	client->tools_list_changed = 0;
	client->negotiated_version[0] = '\0';
	client->server_name[0] = '\0';
	client->server_version[0] = '\0';
	client->supports_tools = 0;
	client->supports_resources = 0;
	client->supports_prompts = 0;
}
