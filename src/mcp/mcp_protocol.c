#include "mcp_internal.h"
#include "errors.h"
#include "nb/log.h"

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

const struct mcp_protocol_adapter *mcp_protocol_borrow(const char *version)
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
		return NB_ERR_INVAL;
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
		return NB_ERR_NOMEM;
	return 0;
}

static int protocol_headers(struct mcp_client *client, const struct mcp_protocol_adapter *adapter,
			    const char *method, cJSON *params, nb_vec *headers)
{
	/* The legacy version is not negotiated until initialize has completed. */
	if (!adapter->per_request && strcmp(method, "initialize") == 0)
		return 0;
	int rc = mcp_protocol_add_header_copy(headers, "MCP-Protocol-Version", adapter->version);

	if (rc != 0)
		return rc;
	if (!adapter->per_request) {
		if (client->session_id)
			return mcp_protocol_add_header_copy(headers, "Mcp-Session-Id",
							    client->session_id);
		return 0;
	}
	rc = mcp_protocol_add_header_copy(headers, "Mcp-Method", method);
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
			return nb_err_trace(client ? client->config.logger : NULL, NB_ERR_INVAL,
					    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
					    __LINE__);
		rc = mcp_protocol_add_header_copy(headers, "Mcp-Name", name->valuestring);
		if (rc != 0)
			return rc;
	}
	if (strcmp(method, "tools/call") == 0)
		return mcp_protocol_tool_headers_copy(client, params, headers);
	return 0;
}

int mcp_protocol_encode(struct mcp_client *client, const struct mcp_protocol_adapter *adapter,
			int id, const char *method, const char *params_json,
			struct mcp_wire_request *out)
{
	if (!client || !adapter || !method || !out)
		return nb_err_trace(client ? client->config.logger : NULL, NB_ERR_INVAL,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	if (out->json || ((void *)out->headers.data))
		return NB_ERR_EXISTS;
	struct mcp_wire_request encoded NB_CLEANUP(mcp_wire_request_cleanup) = {0};
	encoded.id = id;
	int rc = (nb_vec_init(&encoded.headers, sizeof(encoded.json)),
		  nb_vec_reserve(&encoded.headers, 8) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	cJSON *params = params_json && params_json[0] ?
		cJSON_ParseWithOpts(params_json, NULL, 1) : cJSON_CreateObject();

	if (!cJSON_IsObject(params)) {
		cJSON_Delete(params);
		return nb_err_trace(client ? client->config.logger : NULL, NB_ERR_PARSE,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	}
	if (adapter->per_request)
		rc = modern_metadata(params);
	if (rc == 0 && client->config.transport == MCP_TRANSPORT_STREAMABLE_HTTP)
		rc = protocol_headers(client, adapter, method, params, &encoded.headers);
	cJSON *request = cJSON_CreateObject();

	if (rc == 0 && (!request ||
	    !cJSON_AddStringToObject(request, "jsonrpc", "2.0") ||
	    !cJSON_AddStringToObject(request, "method", method) ||
	    (id >= 0 && !cJSON_AddNumberToObject(request, "id", id))))
		rc = nb_err_trace(client ? client->config.logger : NULL, NB_ERR_NOMEM,
				  morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				  __LINE__);
	if (rc == 0 && (adapter->per_request || (params_json && params_json[0]))) {
		if (cJSON_AddItemToObject(request, "params", params))
			params = NULL;
		else
			rc = nb_err_trace(client ? client->config.logger : NULL, NB_ERR_NOMEM,
					  morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
					  __LINE__);
	}
	if (rc == 0) {
		encoded.json = cJSON_PrintUnformatted(request);
		if (!encoded.json)
			rc = nb_err_trace(client ? client->config.logger : NULL, NB_ERR_NOMEM,
					  morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
					  __LINE__);
	}
	cJSON_Delete(params);
	cJSON_Delete(request);
	if (rc == 0) {
		*out = encoded;
		memset(&encoded, 0, sizeof(encoded));
	}
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

void mcp_wire_request_cleanup(struct mcp_wire_request *request)
{
	if (!request)
		return;
	char **header;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&request->headers) &&
			     (header = (char **)nb_vec_at(&request->headers, _i_));
	     _i_++)
		free(*header);
	nb_vec_cleanup(&request->headers);
	cJSON_free(request->json);
	memset(request, 0, sizeof(*request));
}

void mcp_wire_reply_cleanup(struct mcp_wire_reply *reply)
{
	if (!reply)
		return;
	free(reply->json);
	free(reply->session_id);
	memset(reply, 0, sizeof(*reply));
}

/* Caller holds client->lock for the whole exchange, including session updates. */
static int protocol_exchange(struct mcp_client *client,
	const struct mcp_protocol_adapter *adapter, int id, const char *method,
	const char *params, int timeout_ms, struct mcp_wire_reply *reply)
{
	struct mcp_wire_request request NB_CLEANUP(mcp_wire_request_cleanup) = {0};
	int rc;

	rc = mcp_protocol_encode(client, adapter, id, method, params, &request);
	if (rc == 0) {
		if (client->config.transport == MCP_TRANSPORT_STDIO)
			rc = mcp_stdio_exchange(client, &request, timeout_ms, reply);
		else
			rc = mcp_http_exchange(client, &request, timeout_ms, reply);
	}
	return rc;
}

int mcp_protocol_decode(struct nb_log *logger, const struct mcp_wire_reply *reply, int id,
			cJSON **out)
{
	if (!reply || !out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	if (reply->http_status == 401 || reply->http_status == 403)
		return nb_err_trace(logger, NB_ERR_PERMISSION, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	if (reply->http_status >= 500)
		return nb_err_trace(logger, MORPH_ERR_API, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	cJSON *object = reply->json ? cJSON_ParseWithOpts(reply->json, NULL, 1) : NULL;

	if (!object)
		return nb_err_trace(logger, NB_ERR_PARSE, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
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
		return nb_err_trace(logger, NB_ERR_PROTOCOL, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	}
	*out = object;
	if (error || reply->http_status >= 400)
		return nb_err_trace(logger, NB_ERR_PROTOCOL, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	return 0;
}

static int result_type(struct nb_log *logger, const struct mcp_protocol_adapter *adapter,
		       cJSON *result)
{
	if (!adapter->per_request)
		return 0;
	cJSON *type = cJSON_GetObjectItemCaseSensitive(result, "resultType");

	if (!type || (cJSON_IsString(type) && strcmp(type->valuestring, "complete") == 0))
		return 0;
	if (cJSON_IsString(type) && strcmp(type->valuestring, "input_required") == 0) {
		nb_log_write(logger, NB_LOG_WARN,
			     "mcp: server requested input without a supported client capability");
		return nb_err_trace(logger, NB_ERR_UNSUPPORTED, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	}
	return nb_err_trace(logger, NB_ERR_PROTOCOL, morph_error_descriptions(), MORPH_ERROR_COUNT,
			    __FILE__, __LINE__);
}

/* Normalize JSON allocator storage to the request API's malloc-owned text. */
static char *request_result_copy(const cJSON *value)
{
	char *json NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(value);

	return json ? strdup(json) : NULL;
}

static int request_locked(struct mcp_client *client, int id, const char *method,
	const char *params, char **out)
{
	const struct mcp_protocol_adapter *adapter = client->protocol;

	if (!adapter)
		return nb_err_trace(client ? client->config.logger : NULL,
				    MORPH_ERR_NOT_INITIALIZED, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	struct mcp_wire_reply reply = {0};
	cJSON *object = NULL;
	int rc = protocol_exchange(client, adapter, id, method, params,
		mcp_request_timeout_ms(client), &reply);

	if (rc == 0)
		rc = mcp_protocol_decode(client->config.logger, &reply, id, &object);
	if (rc == 0) {
		cJSON *result = cJSON_GetObjectItemCaseSensitive(object, "result");

		rc = result_type(client ? client->config.logger : NULL, adapter, result);
		if (rc == 0 && out) {
			*out = request_result_copy(result);
			if (!*out)
				rc = nb_err_trace(client ? client->config.logger : NULL,
						  NB_ERR_NOMEM, morph_error_descriptions(),
						  MORPH_ERROR_COUNT, __FILE__, __LINE__);
		}
	} else if (object) {
		cJSON *error = cJSON_GetObjectItemCaseSensitive(object, "error");
		cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
		cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");

		/* Only the validated remote error denotes an absent RPC method. */
		if (rc == NB_ERR_PROTOCOL && cJSON_IsNumber(code)) {
			nb_log_write(client ? client->config.logger : NULL, NB_LOG_WARN,
				     "mcp: %s on '%s': JSON-RPC %d: %s", method,
				     client->config.name, code->valueint, message->valuestring);
			if (code->valueint == -32601)
				rc = nb_err_trace(client ? client->config.logger : NULL,
						  NB_ERR_UNSUPPORTED, morph_error_descriptions(),
						  MORPH_ERROR_COUNT, __FILE__, __LINE__);
		}
		if (out)
			*out = request_result_copy(object);
	}
	if (rc != 0)
		nb_log_write(client ? client->config.logger : NULL, NB_LOG_WARN,
			     "mcp: %s on '%s' failed: %s", method, client->config.name,
			     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	cJSON_Delete(object);
	mcp_wire_reply_cleanup(&reply);
	return rc;
}

int mcp_request(struct mcp_client *client, const char *method,
	const char *params, char **result)
{
	if (!client || !method)
		return nb_err_trace(client ? client->config.logger : NULL, NB_ERR_INVAL,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	if (result && *result)
		return NB_ERR_EXISTS;
	nb_mutex_lock(client->lock);
	int rc;

	if (client->next_req_id >= INT_MAX)
		rc = NB_ERR_RANGE;
	else
		rc = request_locked(client, client->next_req_id++, method, params, result);
	nb_mutex_unlock(client->lock);
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
	const struct mcp_protocol_adapter *modern = mcp_protocol_borrow(MCP_PROTOCOL_VERSION);
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
		rc = mcp_protocol_decode(client->config.logger, &reply, id, &object);
	cJSON *evidence = reply.json ? cJSON_Parse(reply.json) : NULL;
	cJSON *error = cJSON_GetObjectItemCaseSensitive(evidence, "error");
	cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
	int modern_error =
		rc != NB_ERR_PERMISSION && reply.http_status < 500 && cJSON_IsNumber(code) &&
		(code->valueint == -32020 || code->valueint == -32021 || code->valueint == -32022);

	if (modern_error) {
		/* This revision is our only modern codec: never negotiate an unknown date. */
		nb_log_write(client ? client->config.logger : NULL, NB_LOG_WARN,
			     "mcp: modern discovery rejected by '%s': %s", client->config.name,
			     reply.json ? reply.json : "missing response");
		rc = nb_err_trace(client ? client->config.logger : NULL,
				  code->valueint == -32022 ? NB_ERR_UNSUPPORTED : NB_ERR_PROTOCOL,
				  morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				  __LINE__);
	} else if (rc == 0) {
		cJSON *result = cJSON_GetObjectItemCaseSensitive(object, "result");
		cJSON *versions = cJSON_GetObjectItemCaseSensitive(result, "supportedVersions");
		cJSON *caps = cJSON_GetObjectItemCaseSensitive(result, "capabilities");

		rc = result_type(client ? client->config.logger : NULL, modern, result);
		if (rc == 0 && (!cJSON_IsArray(versions) || !cJSON_IsObject(caps)))
			rc = nb_err_trace(client ? client->config.logger : NULL, NB_ERR_PROTOCOL,
					  morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
					  __LINE__);
		if (rc == 0 && !version_list_contains(versions, modern->version))
			rc = nb_err_trace(client ? client->config.logger : NULL, NB_ERR_UNSUPPORTED,
					  morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
					  __LINE__);
		if (rc == 0) {
			client->protocol = modern;
			server_details(client, result, 1);
		}
	} else if ((stdio_transport && (rc == NB_ERR_TIMEOUT || rc == NB_ERR_BROKEN_PIPE ||
					(rc == NB_ERR_PROTOCOL && error))) ||
		   (!stdio_transport &&
		    ((reply.http_status >= 400 && reply.http_status < 500 &&
		      reply.http_status != 401 && reply.http_status != 403) ||
		     (rc == NB_ERR_PROTOCOL && object && error && reply.http_status < 400)))) {
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
	nb_buf params NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&params), nb_buf_reserve(&params, 128) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	rc = (nb_buf_printf(&params,
			    "{\"protocolVersion\":\"%s\","
			    "\"capabilities\":{},\"clientInfo\":{\"name\":\"morph\","
			    "\"version\":\"" MORPH_VERSION "\"}}",
			    legacy->version)
		      ? 0
		      : NB_ERR_NOMEM);
	if (rc != 0) {
		nb_buf_cleanup(&params);
		return rc;
	}
	struct mcp_wire_reply reply = {0};
	cJSON *object = NULL;
	int id = client->next_req_id++;
	rc = protocol_exchange(client, legacy, id, "initialize", nb_buf_cstr(&params),
			       mcp_request_timeout_ms(client), &reply);
	nb_buf_cleanup(&params);

	if (rc == 0)
		rc = mcp_protocol_decode(client->config.logger, &reply, id, &object);
	if (rc == 0) {
		cJSON *result = cJSON_GetObjectItemCaseSensitive(object, "result");
		cJSON *version = cJSON_GetObjectItemCaseSensitive(result, "protocolVersion");
		cJSON *caps = cJSON_GetObjectItemCaseSensitive(result, "capabilities");
		const struct mcp_protocol_adapter *selected =
			cJSON_IsString(version) ? mcp_protocol_borrow(version->valuestring) : NULL;

		if (!selected || selected->per_request || !cJSON_IsObject(caps) ||
		    (pinned && selected != legacy) ||
		    (client->config.transport == MCP_TRANSPORT_STREAMABLE_HTTP &&
		    strcmp(selected->version, "2024-11-05") == 0)) {
			rc = nb_err_trace(client ? client->config.logger : NULL, NB_ERR_UNSUPPORTED,
					  morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
					  __LINE__);
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
		rc = nb_err_trace(client ? client->config.logger : NULL, MORPH_ERR_API,
				  morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				  __LINE__);
	mcp_wire_reply_cleanup(&reply);
	return rc;
}

int mcp_protocol_initialize(struct mcp_client *client)
{
	int legacy = 0;
	int rc;

	if (!client)
		return nb_err_trace(client ? client->config.logger : NULL, NB_ERR_INVAL,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	const char *version = client->config.protocol_version;
	int pinned = version[0] && strcmp(version, "auto") != 0;
	const struct mcp_protocol_adapter *selected = pinned ? mcp_protocol_borrow(version) : NULL;

	if (pinned && (!selected ||
	    (client->config.transport == MCP_TRANSPORT_STREAMABLE_HTTP &&
	    strcmp(selected->version, "2024-11-05") == 0)))
		return nb_err_trace(client ? client->config.logger : NULL, NB_ERR_UNSUPPORTED,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	nb_mutex_lock(client->lock);
	mcp_protocol_reset(client);
	client->next_req_id = 1;
	if (pinned && !selected->per_request)
		rc = legacy_initialize(client, selected, 1);
	else
		rc = modern_probe(client, &legacy);
	if (rc == 0 && legacy && pinned)
		rc = nb_err_trace(client ? client->config.logger : NULL, NB_ERR_UNSUPPORTED,
				  morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				  __LINE__);
	if (rc == 0 && client->interrupt_check &&
	    client->interrupt_check(client->interrupt_user_data))
		rc = nb_err_trace(client ? client->config.logger : NULL, NB_ERR_CANCELLED,
				  morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				  __LINE__);
	if (rc == 0 && legacy) {
		if (client->config.transport == MCP_TRANSPORT_STDIO) {
			/* Legacy servers may exit or retain probe state. Start a clean process. */
			mcp_stdio_disconnect(client);
			if (client->interrupt_check &&
			    client->interrupt_check(client->interrupt_user_data))
				rc = NB_ERR_CANCELLED;
			else
				rc = mcp_stdio_connect(client);
			client->next_req_id = 1;
		}
		if (rc == 0)
			rc = legacy_initialize(client,
					       mcp_protocol_borrow(MCP_LEGACY_PROTOCOL_VERSION), 0);
	}
	if (rc == 0) {
		snprintf(client->negotiated_version, sizeof(client->negotiated_version),
			"%s", client->protocol->version);
		client->connected = 1;
		nb_log_write(client ? client->config.logger : NULL, NB_LOG_INFO,
			     "mcp: '%s' selected protocol %s", client->config.name,
			     client->negotiated_version);
	}
	nb_mutex_unlock(client->lock);
	return rc;
}

void mcp_protocol_reset(struct mcp_client *client)
{
	if (client->tool_schemas) {
		nb_map_iter iter;
		nb_str key;
		void *value;

		nb_map_iter_init(client->tool_schemas, &iter);
		while (nb_map_iter_next(&iter, &key, &value))
			cJSON_Delete((cJSON *)value);
		nb_map_free(client->tool_schemas);
		client->tool_schemas = NULL;
	}
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
