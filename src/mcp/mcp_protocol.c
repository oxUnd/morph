#include "mcp_internal.h"
#include "errors.h"
#include <nb/log.h>
#include <nb/time.h>

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MCP_REQUEST_TIMEOUT_MS 30000
#define MCP_PROBE_TIMEOUT_MS 5000
#define MCP_META_PREFIX "io.modelcontextprotocol/"

/* Known revisions, newest first; an unknown date never selects a codec. */
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

static int modern_metadata(struct mcp_client *client, cJSON *params)
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
	cJSON *capabilities NB_CLEANUP(cJSON_DeleteOwner) =
		mcp_client_capabilities_create(client, mcp_protocol_borrow(MCP_PROTOCOL_VERSION));

	if (!info || !capabilities || !cJSON_AddStringToObject(info, "name", "morph") ||
	    !cJSON_AddStringToObject(info, "version", MORPH_VERSION) ||
	    !cJSON_AddStringToObject(meta, MCP_META_PREFIX "protocolVersion",
				     MCP_PROTOCOL_VERSION) ||
	    !cJSON_AddItemToObject(meta, MCP_META_PREFIX "clientCapabilities", capabilities))
		return NB_ERR_NOMEM;

	capabilities = NULL;
	if (client->log_level[0]) {
		cJSON_DeleteItemFromObjectCaseSensitive(meta, MCP_META_PREFIX "logLevel");
		if (!cJSON_AddStringToObject(meta, MCP_META_PREFIX "logLevel", client->log_level))
			return NB_ERR_NOMEM;
	}

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
		rc = modern_metadata(client, params);
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
	    (client->connecting || (client->interrupt_check && !client->request_context)))
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
		const struct mcp_protocol_adapter *previous = client->protocol;
		client->protocol = adapter;
		if (client->config.transport == MCP_TRANSPORT_STDIO)
			rc = mcp_stdio_exchange(client, &request, timeout_ms, reply);
		else
			rc = mcp_http_exchange(client, &request, timeout_ms, reply);

		client->protocol = previous;
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
		return NB_ERR_AGAIN;
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

static void request_cancel_locked(struct mcp_client *client, int id)
{
	if (client->config.transport == MCP_TRANSPORT_STDIO && client->stdin_fd < 0)
		return;

	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!params || !cJSON_AddNumberToObject(params, "requestId", id) ||
	    !cJSON_AddStringToObject(params, "reason", "Client stopped waiting for this request"))
		return;

	char *encoded NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(params);
	if (!encoded)
		return;

	struct mcp_wire_reply reply NB_CLEANUP(mcp_wire_reply_cleanup) = {0};
	int (*interrupt)(void *) = client->interrupt_check;
	client->interrupt_check = NULL;
	(void)protocol_exchange(client, client->protocol, -1, "notifications/cancelled", encoded,
				200, &reply);
	client->interrupt_check = interrupt;
}

static int request_once_locked(struct mcp_client *client, int id, const char *method,
			       const char *params, int timeout, char **out)
{
	const struct mcp_protocol_adapter *adapter = client->protocol;

	if (!adapter)
		return nb_err_trace(client ? client->config.logger : NULL,
				    MORPH_ERR_NOT_INITIALIZED, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	struct mcp_wire_reply reply = {0};
	cJSON *object = NULL;
	client->active_request_id = id;
	client->peer_cancelled = 0;
	int rc = protocol_exchange(client, adapter, id, method, params, timeout, &reply);
	if ((rc == NB_ERR_CANCELLED || rc == NB_ERR_TIMEOUT) && !client->peer_cancelled)
		request_cancel_locked(client, id);

	client->active_request_id = 0;

	if (rc == 0)
		rc = mcp_protocol_decode(client->config.logger, &reply, id, &object);
	if (rc == 0) {
		cJSON *result = cJSON_GetObjectItemCaseSensitive(object, "result");

		rc = result_type(client ? client->config.logger : NULL, adapter, result);
		if ((rc == 0 || rc == NB_ERR_AGAIN) && out) {
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
	if (rc != 0 && rc != NB_ERR_AGAIN)
		nb_log_write(client ? client->config.logger : NULL, NB_LOG_WARN,
			     "mcp: %s on '%s' failed: %s", method, client->config.name,
			     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	cJSON_Delete(object);
	mcp_wire_reply_cleanup(&reply);
	return rc;
}

static int resume_params_create(struct mcp_client *client, const char *params, const cJSON *result,
				char **out)
{
	const cJSON *requests = cJSON_GetObjectItemCaseSensitive(result, "inputRequests");
	const cJSON *state = cJSON_GetObjectItemCaseSensitive(result, "requestState");
	if ((!requests && !state) || (requests && !cJSON_IsObject(requests)) ||
	    (state && !cJSON_IsString(state)))
		return NB_ERR_PROTOCOL;

	cJSON *next NB_CLEANUP(cJSON_DeleteOwner) =
		params ? cJSON_ParseWithOpts(params, NULL, 1) : cJSON_CreateObject();
	cJSON *responses NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!next || !responses)
		return NB_ERR_NOMEM;

	const cJSON *input;
	cJSON_ArrayForEach (input, requests) {
		const char *method =
			cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input, "method"));
		const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(input, "params");
		cJSON *reply NB_CLEANUP(cJSON_DeleteOwner) = NULL;
		if (!method || !input->string || (arguments && !cJSON_IsObject(arguments)))
			return NB_ERR_PROTOCOL;

		if (strcmp(method, "elicitation/create") &&
		    strcmp(method, "sampling/createMessage") && strcmp(method, "roots/list"))
			return NB_ERR_PROTOCOL;

		int rc = mcp_handle_input_request(client, method, arguments, &reply);
		if (rc != 0)
			return rc;

		if (!cJSON_AddItemToObject(responses, input->string, reply))
			return NB_ERR_NOMEM;

		reply = NULL;
	}

	cJSON_DeleteItemFromObjectCaseSensitive(next, "inputResponses");
	cJSON_DeleteItemFromObjectCaseSensitive(next, "requestState");
	if (requests) {
		if (!cJSON_AddItemToObject(next, "inputResponses", responses))
			return NB_ERR_NOMEM;

		responses = NULL;
	}

	if (state && !cJSON_AddStringToObject(next, "requestState", state->valuestring))
		return NB_ERR_NOMEM;

	char *encoded NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(next);
	if (!encoded)
		return NB_ERR_NOMEM;

	*out = strdup(encoded);
	return *out ? 0 : NB_ERR_NOMEM;
}

static int request_locked(struct mcp_client *client, int id, const char *method, const char *params,
			  char **out)
{
	char *resumed NB_CLEANUP(nb_charp_cleanup) = NULL;
	int64_t started = nb_time_monotonic_ms();
	int64_t waited = client->interaction_wait_ms;
	int timeout = mcp_request_timeout_ms(client);

	for (;;) {
		if (mcp_client_interrupted(client))
			return NB_ERR_CANCELLED;

		int64_t remaining = timeout - (nb_time_monotonic_ms() - started -
					       (client->interaction_wait_ms - waited));
		if (remaining <= 0)
			return NB_ERR_TIMEOUT;

		char *text NB_CLEANUP(nb_charp_cleanup) = NULL;
		int rc = request_once_locked(client, id, method, resumed ? resumed : params,
					     (int)remaining, &text);
		if (rc != NB_ERR_AGAIN) {
			if (out)
				*out = nb_charp_steal(&text);

			return rc;
		}

		if (strcmp(method, "tools/call") && strcmp(method, "resources/read") &&
		    strcmp(method, "prompts/get"))
			return NB_ERR_UNSUPPORTED;

		if (nb_time_monotonic_ms() - started - (client->interaction_wait_ms - waited) >=
		    timeout)
			return NB_ERR_TIMEOUT;

		cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(text);
		const cJSON *pending = cJSON_GetObjectItemCaseSensitive(result, "inputRequests");
		if (!pending || !pending->child) {
			struct http_control control = {
				.interrupt_check = mcp_client_interrupted,
				.interrupt_user_data = client,
			};
			rc = http_wait_cancelable(&control, 25);
			if (rc != 0)
				return rc;
		}

		char *next NB_CLEANUP(nb_charp_cleanup) = NULL;
		rc = resume_params_create(client, resumed ? resumed : params, result, &next);
		if (rc != 0)
			return rc;

		free(resumed);
		resumed = nb_charp_steal(&next);
		if (client->next_req_id >= INT_MAX)
			return NB_ERR_RANGE;

		id = client->next_req_id++;
	}
}

static int request_interrupted(void *opaque)
{
	const struct http_control *control = opaque;
	return (control->cancel_flag && nb_atomic_int_load(control->cancel_flag)) ||
	       nb_cancel_requested(control->cancel_token) ||
	       (control->interrupt_check && control->interrupt_check(control->interrupt_user_data));
}

int mcp_request_controlled(struct mcp_client *client, const char *method, const char *params,
			   const void *context, const struct http_control *control, char **result)
{
	if (!client || !method)
		return nb_err_trace(client ? client->config.logger : NULL, NB_ERR_INVAL,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	if (result && *result)
		return NB_ERR_EXISTS;
	int lock_status = nb_mutex_trylock(client->lock);
	while (lock_status == NB_ERR_BUSY) {
		if (nb_cancel_requested(client->shutdown_cancel) ||
		    (control && request_interrupted((void *)control)) ||
		    (!context && nb_atomic_int_load(&client->refreshing_catalog) &&
		     client->catalog_cancelled && nb_atomic_int_load(client->catalog_cancelled)))
			return NB_ERR_CANCELLED;

		int wait_status = http_wait_cancelable(control, 10);
		if (wait_status != 0)
			return wait_status;

		lock_status = nb_mutex_trylock(client->lock);
	}

	if (lock_status != 0)
		return lock_status;

	int rc;
	const void *previous_context = client->request_context;
	int (*previous_interrupt)(void *) = client->interrupt_check;
	void *previous_interrupt_data = client->interrupt_user_data;
	client->request_context = context;
	if (control) {
		client->interrupt_check = request_interrupted;
		client->interrupt_user_data = (void *)control;
	}

	if (client->next_req_id >= INT_MAX)
		rc = NB_ERR_RANGE;
	else
		rc = request_locked(client, client->next_req_id++, method, params, result);

	client->request_context = previous_context;
	client->peer_cancelled = 0;
	client->interrupt_check = previous_interrupt;
	client->interrupt_user_data = previous_interrupt_data;
	nb_mutex_unlock(client->lock);
	return rc;
}

int mcp_request_with_context(struct mcp_client *client, const char *method, const char *params,
			     const void *context, char **result)
{
	return mcp_request_controlled(client, method, params, context, NULL, result);
}

int mcp_request(struct mcp_client *client, const char *method, const char *params, char **result)
{
	return mcp_request_with_context(client, method, params, NULL, result);
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

static const struct mcp_protocol_adapter *discovered_legacy_borrow(
	cJSON *versions, enum mcp_transport_type transport)
{
	for (size_t i = 0; i < sizeof(adapters) / sizeof(adapters[0]); i++) {
		const struct mcp_protocol_adapter *adapter = &adapters[i];

		if (adapter->per_request ||
		    (transport == MCP_TRANSPORT_STREAMABLE_HTTP &&
		     strcmp(adapter->version, "2024-11-05") == 0))
			continue;

		if (version_list_contains(versions, adapter->version))
			return adapter;
	}

	return NULL;
}

static int server_details(struct mcp_client *client, cJSON *result, int modern)
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
	cJSON *instructions = cJSON_GetObjectItemCaseSensitive(result, "instructions");
	char *copied NB_CLEANUP(nb_charp_cleanup) =
		cJSON_IsString(instructions) ? strdup(instructions->valuestring) : NULL;

	if (cJSON_IsString(instructions) && !copied)
		return NB_ERR_NOMEM;

	free(client->instructions);
	client->instructions = nb_charp_steal(&copied);

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
	const char *catalogs[] = {"tools", "resources", "prompts"};
	client->supports_catalog_notifications = 0;
	for (size_t i = 0; i < sizeof(catalogs) / sizeof(catalogs[0]); i++) {
		const cJSON *catalog = cJSON_GetObjectItemCaseSensitive(caps, catalogs[i]);
		if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(catalog, "listChanged")))
			client->supports_catalog_notifications = 1;
	}

	return 0;
}

/* Only positive legacy evidence permits downgrade; auth/outages never do. */
static int modern_probe(struct mcp_client *client,
	const struct mcp_protocol_adapter **legacy)
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

	*legacy = NULL;
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
		if (rc == 0 && !version_list_contains(versions, modern->version)) {
			*legacy = discovered_legacy_borrow(versions, client->config.transport);
			if (!*legacy)
				rc = nb_err_trace(client->config.logger, NB_ERR_UNSUPPORTED,
					morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
					__LINE__);
		}

		if (rc == 0 && !*legacy) {
			client->protocol = modern;
			rc = server_details(client, result, 1);
		}
	} else if ((stdio_transport && (rc == NB_ERR_TIMEOUT || rc == NB_ERR_BROKEN_PIPE ||
					(rc == NB_ERR_PROTOCOL && error))) ||
		   (!stdio_transport &&
		    ((reply.http_status >= 400 && reply.http_status < 500 &&
		      reply.http_status != 401 && reply.http_status != 403) ||
		     (rc == NB_ERR_PROTOCOL && object && error && reply.http_status < 400)))) {
		*legacy = mcp_protocol_borrow(MCP_LEGACY_PROTOCOL_VERSION);
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
	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *capabilities NB_CLEANUP(cJSON_DeleteOwner) =
		mcp_client_capabilities_create(client, legacy);
	if (!params || !capabilities ||
	    !cJSON_AddStringToObject(params, "protocolVersion", legacy->version) ||
	    !cJSON_AddItemToObject(params, "capabilities", capabilities))
		return NB_ERR_NOMEM;

	capabilities = NULL;
	cJSON *info = cJSON_AddObjectToObject(params, "clientInfo");
	if (!info || !cJSON_AddStringToObject(info, "name", "morph") ||
	    !cJSON_AddStringToObject(info, "version", MORPH_VERSION))
		return NB_ERR_NOMEM;

	char *encoded NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(params);
	if (!encoded)
		return NB_ERR_NOMEM;

	struct mcp_wire_reply reply = {0};
	cJSON *object = NULL;
	int id = client->next_req_id++;
	int rc = protocol_exchange(client, legacy, id, "initialize", encoded,
				   mcp_request_timeout_ms(client), &reply);

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
			rc = server_details(client, result, 0);
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
	const struct mcp_protocol_adapter *legacy = NULL;
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
	if (client->connected || client->listener || client->subscription_id > 0) {
		nb_mutex_unlock(client->lock);
		return NB_ERR_EXISTS;
	}

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
	if (rc == 0 && mcp_client_interrupted(client))
		rc = nb_err_trace(client ? client->config.logger : NULL, NB_ERR_CANCELLED,
				  morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				  __LINE__);
	if (rc == 0 && legacy) {
		if (client->config.transport == MCP_TRANSPORT_STDIO) {
			/* Legacy servers may exit or retain probe state. Start a clean process. */
			mcp_stdio_disconnect(client);
			if (mcp_client_interrupted(client))
				rc = NB_ERR_CANCELLED;
			else
				rc = mcp_stdio_connect(client);
			client->next_req_id = 1;
		}
		if (rc == 0)
			rc = legacy_initialize(client, legacy, 0);
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
	free(client->instructions);
	client->instructions = NULL;
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
	client->resources_list_changed = 0;
	client->prompts_list_changed = 0;
	client->peer_cancelled = 0;
	client->negotiated_version[0] = '\0';
	client->server_name[0] = '\0';
	client->server_version[0] = '\0';
	client->supports_tools = 0;
	client->supports_resources = 0;
	client->supports_prompts = 0;
	client->supports_catalog_notifications = 0;
}
