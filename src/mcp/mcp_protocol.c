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

static int modern_metadata(cJSON *params)
{
	cJSON *meta = cJSON_GetObjectItemCaseSensitive(params, "_meta");

	if (!meta)
		meta = cJSON_AddObjectToObject(params, "_meta");
	if (!cJSON_IsObject(meta))
		MORPH_RETURN(-EINVAL);
	const char *keys[] = {MCP_META_PREFIX "protocolVersion",
		MCP_META_PREFIX "clientInfo", MCP_META_PREFIX "clientCapabilities"};

	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		while (cJSON_GetObjectItemCaseSensitive(meta, keys[i]))
			cJSON_DeleteItemFromObjectCaseSensitive(meta, keys[i]);
	}
	cJSON *info = cJSON_AddObjectToObject(meta, MCP_META_PREFIX "clientInfo");

	if (!info || !cJSON_AddStringToObject(info, "name", "morph") ||
	    !cJSON_AddStringToObject(info, "version", MORPH_VERSION) ||
	    !cJSON_AddStringToObject(meta, MCP_META_PREFIX "protocolVersion",
		MCP_PROTOCOL_VERSION) ||
	    !cJSON_AddObjectToObject(meta, MCP_META_PREFIX "clientCapabilities"))
		MORPH_RETURN(-ENOMEM);
	return 0;
}

static int protocol_headers(struct mcp_client *client,
	const struct mcp_protocol_adapter *adapter, const char *method,
	cJSON *params, struct arena *arena, morph_array_t *headers)
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
		cJSON *name = cJSON_GetObjectItemCaseSensitive(params, key);

		if (!cJSON_IsString(name))
			MORPH_RETURN(-EINVAL);
		rc = mcp_protocol_add_header(arena, headers, "Mcp-Name", name->valuestring);
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
		MORPH_RETURN(-EINVAL);
	memset(out, 0, sizeof(*out));
	out->id = id;
	int rc = morph_array_init(&out->headers, 8, sizeof(out->json));

	if (rc != 0)
		return rc;
	cJSON *params = params_json && params_json[0] ?
		cJSON_ParseWithOpts(params_json, NULL, 1) : cJSON_CreateObject();

	if (!cJSON_IsObject(params)) {
		cJSON_Delete(params);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}
	if (adapter->per_request)
		rc = modern_metadata(params);
	if (rc == 0 && client->config.transport == MCP_TRANSPORT_STREAMABLE_HTTP)
		rc = protocol_headers(client, adapter, method, params, arena, &out->headers);
	cJSON *request = cJSON_CreateObject();

	if (rc == 0 && (!request ||
	    !cJSON_AddStringToObject(request, "jsonrpc", "2.0") ||
	    !cJSON_AddStringToObject(request, "method", method) ||
	    (id >= 0 && !cJSON_AddNumberToObject(request, "id", id))))
		MORPH_SET_ERR(rc, -ENOMEM);
	if (rc == 0 && (adapter->per_request || (params_json && params_json[0]))) {
		if (cJSON_AddItemToObject(request, "params", params))
			params = NULL;
		else
			MORPH_SET_ERR(rc, -ENOMEM);
	}
	if (rc == 0) {
		char *json = cJSON_PrintUnformatted(request);

		out->json = json ? arena_strdup(arena, json) : NULL;
		free(json);
		if (!out->json)
			MORPH_SET_ERR(rc, -ENOMEM);
	}
	cJSON_Delete(params);
	cJSON_Delete(request);
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
		MORPH_RETURN(-ENOMEM);
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

int mcp_protocol_decode(const struct mcp_wire_reply *reply, int id,
	cJSON **out)
{
	*out = NULL;
	if (reply->http_status == 401 || reply->http_status == 403)
		MORPH_RETURN(-EACCES);
	if (reply->http_status >= 500)
		MORPH_RETURN(MORPH_ERR_API);
	cJSON *object = reply->json ? cJSON_ParseWithOpts(reply->json, NULL, 1) : NULL;

	if (!object)
		MORPH_RETURN(MORPH_ERR_PARSE);
	cJSON *version = cJSON_GetObjectItemCaseSensitive(object, "jsonrpc");
	cJSON *response_id = cJSON_GetObjectItemCaseSensitive(object, "id");
	cJSON *error = cJSON_GetObjectItemCaseSensitive(object, "error");
	cJSON *result = cJSON_GetObjectItemCaseSensitive(object, "result");
	int matches = cJSON_IsNumber(response_id) && response_id->valuedouble == id;

	/* HTTP header validation can fail before the server reads the request ID. */
	if (!matches && reply->http_status == 400 && !response_id && error)
		matches = 1;
	cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
	cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");
	int valid_error = !error || (cJSON_IsObject(error) && cJSON_IsNumber(code) &&
		code->valuedouble >= INT_MIN && code->valuedouble <= INT_MAX &&
		(double)code->valueint == code->valuedouble && cJSON_IsString(message));

	if (!cJSON_IsObject(object) || !cJSON_IsString(version) ||
	    strcmp(version->valuestring, "2.0") != 0 || !matches || !valid_error ||
	    (!!error == !!result) || (result && !cJSON_IsObject(result))) {
		cJSON_Delete(object);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}
	*out = object;
	if (error || reply->http_status >= 400)
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	return 0;
}

static int result_type(const struct mcp_protocol_adapter *adapter, cJSON *result)
{
	if (!adapter->per_request)
		return 0;
	cJSON *type = cJSON_GetObjectItemCaseSensitive(result, "resultType");

	if (!type || (cJSON_IsString(type) && strcmp(type->valuestring, "complete") == 0))
		return 0;
	if (cJSON_IsString(type) && strcmp(type->valuestring, "input_required") == 0) {
		log_warn("mcp: server requested input without a supported client capability");
		MORPH_RETURN(-ENOTSUP);
	}
	MORPH_RETURN(MORPH_ERR_PROTOCOL);
}

static int request_locked(struct mcp_client *client, int id, const char *method,
	const char *params, char **out)
{
	const struct mcp_protocol_adapter *adapter = client->protocol;

	if (!adapter)
		MORPH_RETURN(MORPH_ERR_NOT_INITIALIZED);
	struct mcp_wire_reply reply = {0};
	cJSON *object = NULL;
	int rc = protocol_exchange(client, adapter, id, method, params,
		mcp_request_timeout_ms(client), &reply);

	if (rc == 0)
		rc = mcp_protocol_decode(&reply, id, &object);
	if (rc == 0) {
		cJSON *result = cJSON_GetObjectItemCaseSensitive(object, "result");

		rc = result_type(adapter, result);
		if (rc == 0 && out) {
			*out = cJSON_PrintUnformatted(result);
			if (!*out)
				MORPH_SET_ERR(rc, -ENOMEM);
		}
	} else if (object) {
		cJSON *error = cJSON_GetObjectItemCaseSensitive(object, "error");
		cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
		cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");

		/* Only the validated remote error denotes an absent RPC method. */
		if (rc == MORPH_ERR_PROTOCOL && cJSON_IsNumber(code)) {
			log_warn("mcp: %s on '%s': JSON-RPC %d: %s", method,
				client->config.name, code->valueint, message->valuestring);
			if (code->valueint == -32601)
				MORPH_SET_ERR(rc, -ENOSYS);
		}
		if (out)
			*out = cJSON_PrintUnformatted(object);
	}
	if (rc != 0)
		log_warn("mcp: %s on '%s' failed: %s", method,
			client->config.name, morph_strerror(rc));
	cJSON_Delete(object);
	mcp_wire_reply_cleanup(&reply);
	return rc;
}

int mcp_request(struct mcp_client *client, const char *method,
	const char *params, char **result)
{
	if (!client || !method)
		MORPH_RETURN(-EINVAL);
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

static int version_list_contains(cJSON *versions, const char *version)
{
	cJSON *item;

	cJSON_ArrayForEach(item, versions) {
		if (cJSON_IsString(item) && strcmp(item->valuestring, version) == 0)
			return 1;
	}
	return 0;
}

static void server_details(struct mcp_client *client, cJSON *result, int modern)
{
	cJSON *caps = cJSON_GetObjectItemCaseSensitive(result, "capabilities");
	cJSON *info;

	if (modern) {
		cJSON *meta = cJSON_GetObjectItemCaseSensitive(result, "_meta");

		info = cJSON_GetObjectItemCaseSensitive(meta, MCP_META_PREFIX "serverInfo");
	} else {
		info = cJSON_GetObjectItemCaseSensitive(result, "serverInfo");
	}
	cJSON *name = cJSON_GetObjectItemCaseSensitive(info, "name");
	cJSON *version = cJSON_GetObjectItemCaseSensitive(info, "version");

	if (cJSON_IsString(name))
		snprintf(client->server_name, sizeof(client->server_name), "%s", name->valuestring);
	if (cJSON_IsString(version))
		snprintf(client->server_version, sizeof(client->server_version), "%s",
			version->valuestring);
	client->supports_tools =
		cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(caps, "tools"));
	client->supports_resources =
		cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(caps, "resources"));
	client->supports_prompts =
		cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(caps, "prompts"));
}

/* Only positive legacy evidence permits downgrade; auth/outages never do. */
static int modern_probe(struct mcp_client *client, int *legacy)
{
	const struct mcp_protocol_adapter *modern = mcp_protocol_find(MCP_PROTOCOL_VERSION);
	struct mcp_wire_reply reply = {0};
	cJSON *object = NULL;
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
		rc = mcp_protocol_decode(&reply, id, &object);
	cJSON *evidence = reply.json ? cJSON_Parse(reply.json) : NULL;
	cJSON *error = cJSON_GetObjectItemCaseSensitive(evidence, "error");
	cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
	int modern_error = rc != -EACCES && reply.http_status < 500 && cJSON_IsNumber(code) &&
		(code->valueint == -32020 || code->valueint == -32021 || code->valueint == -32022);

	if (modern_error) {
		/* This revision is our only modern codec: never negotiate an unknown date. */
		log_warn("mcp: modern discovery rejected by '%s': %s", client->config.name,
			reply.json ? reply.json : "missing response");
		MORPH_SET_ERR(rc, code->valueint == -32022 ? -EPROTONOSUPPORT : MORPH_ERR_PROTOCOL);
	} else if (rc == 0) {
		cJSON *result = cJSON_GetObjectItemCaseSensitive(object, "result");
		cJSON *versions = cJSON_GetObjectItemCaseSensitive(result, "supportedVersions");
		cJSON *caps = cJSON_GetObjectItemCaseSensitive(result, "capabilities");

		rc = result_type(modern, result);
		if (rc == 0 && (!cJSON_IsArray(versions) || !cJSON_IsObject(caps)))
			MORPH_SET_ERR(rc, MORPH_ERR_PROTOCOL);
		if (rc == 0 && !version_list_contains(versions, modern->version))
			MORPH_SET_ERR(rc, -EPROTONOSUPPORT);
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
	cJSON_Delete(evidence);
	cJSON_Delete(object);
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
	cJSON *object = NULL;
	int id = client->next_req_id++;
	rc = protocol_exchange(client, legacy, id, "initialize", morph_buf_cstr(&params),
		mcp_request_timeout_ms(client), &reply);
	morph_buf_cleanup(&params);

	if (rc == 0)
		rc = mcp_protocol_decode(&reply, id, &object);
	if (rc == 0) {
		cJSON *result = cJSON_GetObjectItemCaseSensitive(object, "result");
		cJSON *version = cJSON_GetObjectItemCaseSensitive(result, "protocolVersion");
		cJSON *caps = cJSON_GetObjectItemCaseSensitive(result, "capabilities");
		const struct mcp_protocol_adapter *selected = cJSON_IsString(version) ?
			mcp_protocol_find(version->valuestring) : NULL;

		if (!selected || selected->per_request || !cJSON_IsObject(caps) ||
		    (pinned && selected != legacy) ||
		    (client->config.transport == MCP_TRANSPORT_STREAMABLE_HTTP &&
		    strcmp(selected->version, "2024-11-05") == 0)) {
			MORPH_SET_ERR(rc, -EPROTONOSUPPORT);
		} else {
			client->protocol = selected;
			server_details(client, result, 0);
			client->session_id = reply.session_id;
			reply.session_id = NULL;
		}
	}
	cJSON_Delete(object);
	mcp_wire_reply_cleanup(&reply);
	if (rc != 0)
		return rc;
	/* A notification has no ID and no JSON-RPC response. */
	rc = protocol_exchange(client, client->protocol, -1,
		"notifications/initialized", NULL, mcp_request_timeout_ms(client), &reply);
	if (rc == 0 && reply.http_status >= 400)
		MORPH_SET_ERR(rc, MORPH_ERR_API);
	mcp_wire_reply_cleanup(&reply);
	return rc;
}

int mcp_protocol_initialize(struct mcp_client *client)
{
	int legacy = 0;
	int rc;

	if (!client)
		MORPH_RETURN(-EINVAL);
	const char *version = client->config.protocol_version;
	int pinned = version[0] && strcmp(version, "auto") != 0;
	const struct mcp_protocol_adapter *selected = pinned ? mcp_protocol_find(version) : NULL;

	if (pinned && (!selected ||
	    (client->config.transport == MCP_TRANSPORT_STREAMABLE_HTTP &&
	    strcmp(selected->version, "2024-11-05") == 0)))
		MORPH_RETURN(-EPROTONOSUPPORT);
	pthread_mutex_lock(&client->lock);
	mcp_protocol_reset(client);
	client->next_req_id = 1;
	if (pinned && !selected->per_request)
		rc = legacy_initialize(client, selected, 1);
	else
		rc = modern_probe(client, &legacy);
	if (rc == 0 && legacy && pinned)
		MORPH_SET_ERR(rc, -EPROTONOSUPPORT);
	if (rc == 0 && client->interrupt_check &&
	    client->interrupt_check(client->interrupt_user_data))
		MORPH_SET_ERR(rc, -ECANCELED);
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
		log_info("mcp: '%s' selected protocol %s", client->config.name,
			client->negotiated_version);
	}
	pthread_mutex_unlock(&client->lock);
	return rc;
}

void mcp_protocol_reset(struct mcp_client *client)
{
	for (size_t i = 0; i < client->tool_schemas.cap; i++)
		cJSON_Delete(client->tool_schemas.entries[i].value);
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
