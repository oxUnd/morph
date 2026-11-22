#include "mcp_internal.h"

#include <nb/cleanup.h>
#include <nb/cancel.h>
#include <nb/time.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int capability_add(cJSON *capabilities, const char *name, const char *mode)
{
	cJSON *capability = cJSON_AddObjectToObject(capabilities, name);

	if (!capability)
		return NB_ERR_NOMEM;

	if (mode && !cJSON_AddObjectToObject(capability, mode))
		return NB_ERR_NOMEM;

	return 0;
}

int mcp_client_interrupted(void *opaque)
{
	const struct mcp_client *client = opaque;
	return nb_cancel_requested(client->shutdown_cancel) ||
	       (nb_atomic_int_load(&client->refreshing_catalog) && !client->request_context &&
		client->catalog_cancelled && nb_atomic_int_load(client->catalog_cancelled)) ||
	       (client->interrupt_check && client->interrupt_check(client->interrupt_user_data));
}

cJSON *mcp_client_capabilities_create(const struct mcp_client *client,
				      const struct mcp_protocol_adapter *adapter)
{
	cJSON *capabilities NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

	if (!capabilities || !client || !adapter)
		return NULL;

	if (client->handlers.elicitation && strcmp(adapter->version, "2025-06-18") >= 0) {
		if (capability_add(capabilities, "elicitation", "form") != 0)
			return NULL;

		if (strcmp(adapter->version, "2025-11-25") >= 0 &&
		    !cJSON_AddObjectToObject(
			    cJSON_GetObjectItemCaseSensitive(capabilities, "elicitation"), "url"))
			return NULL;
	}

	if (client->handlers.sampling && capability_add(capabilities, "sampling", NULL) != 0)
		return NULL;

	if (client->handlers.sampling && client->handlers.sampling_tools &&
	    strcmp(adapter->version, "2025-11-25") >= 0 &&
	    !cJSON_AddObjectToObject(cJSON_GetObjectItemCaseSensitive(capabilities, "sampling"),
				     "tools"))
		return NULL;

	if (client->handlers.roots && capability_add(capabilities, "roots", NULL) != 0)
		return NULL;

	cJSON *result = capabilities;
	capabilities = NULL;
	return result;
}

static int request_id_valid(const cJSON *id)
{
	return cJSON_IsString(id) || (cJSON_IsNumber(id) && isfinite(id->valuedouble));
}

int mcp_handle_input_request(struct mcp_client *client, const char *method, const cJSON *params,
			     cJSON **result)
{
	mcp_client_request_fn callback = NULL;

	if (strcmp(method, "ping") == 0) {
		*result = cJSON_CreateObject();
		return *result ? 0 : NB_ERR_NOMEM;
	}

	if (strcmp(method, "elicitation/create") == 0)
		callback = client->handlers.elicitation;
	else if (strcmp(method, "sampling/createMessage") == 0)
		callback = client->handlers.sampling;
	else if (strcmp(method, "roots/list") == 0)
		callback = client->handlers.roots;

	if (!callback)
		return NB_ERR_UNSUPPORTED;

	if (client->protocol && strcmp(method, "elicitation/create") == 0) {
		if (strcmp(client->protocol->version, "2025-06-18") < 0)
			return NB_ERR_UNSUPPORTED;

		const char *mode =
			cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "mode"));
		if (mode && !strcmp(mode, "url")) {
			const cJSON *id = cJSON_GetObjectItemCaseSensitive(params, "elicitationId");
			if (strcmp(client->protocol->version, "2025-11-25") < 0 ||
			    (!client->protocol->per_request &&
			     (!cJSON_IsString(id) || !id->valuestring[0])))
				return NB_ERR_INVAL;
		}
	}

	if (client->protocol && strcmp(method, "sampling/createMessage") == 0 &&
	    strcmp(client->protocol->version, "2025-11-25") < 0 &&
	    (cJSON_HasObjectItem(params, "tools") || cJSON_HasObjectItem(params, "toolChoice")))
		return NB_ERR_UNSUPPORTED;

	int64_t start = nb_time_monotonic_ns();
	int rc = callback(client->config.name, method, params, client->request_context, result,
			  client->handlers.user_data);
	int64_t elapsed = nb_time_monotonic_ns() - start;

	if (elapsed > 0)
		client->interaction_wait_ms += elapsed / INT64_C(1000000);

	if (rc == 0 && !cJSON_IsObject(*result))
		return NB_ERR_PROTOCOL;

	if (rc == 0 && client->protocol && !strcmp(method, "sampling/createMessage") &&
	    strcmp(client->protocol->version, "2025-11-25") < 0) {
		cJSON *content = cJSON_GetObjectItemCaseSensitive(*result, "content");
		if (cJSON_IsArray(content)) {
			if (cJSON_GetArraySize(content) != 1)
				return NB_ERR_UNSUPPORTED;

			cJSON *part = cJSON_DetachItemFromArray(content, 0);
			if (!cJSON_ReplaceItemInObjectCaseSensitive(*result, "content", part)) {
				cJSON_Delete(part);
				return NB_ERR_NOMEM;
			}
		}
	}

	return rc;
}

static int response_create(const cJSON *id, int status, cJSON **result, char **out)
{
	cJSON *response NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *copied_id NB_CLEANUP(cJSON_DeleteOwner) =
		id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull();

	if (!response || !copied_id || !cJSON_AddStringToObject(response, "jsonrpc", "2.0"))
		return NB_ERR_NOMEM;

	if (!cJSON_AddItemToObject(response, "id", copied_id))
		return NB_ERR_NOMEM;

	copied_id = NULL;
	if (status == 0) {
		if (!cJSON_AddItemToObject(response, "result", *result))
			return NB_ERR_NOMEM;

		*result = NULL;
	} else {
		int code = status == NB_ERR_FORMAT					? -32600
			   : status == NB_ERR_UNSUPPORTED				? -32601
			   : status == NB_ERR_INVAL || status == NB_ERR_PROTOCOL	? -32602
			   : status == NB_ERR_PERMISSION				? -32000
			   : status == NB_ERR_CANCELLED || status == NB_ERR_INTERRUPTED ? -32800
											: -32603;
		const char *message = code == -32600   ? "Invalid request"
				      : code == -32601 ? "Method not found"
				      : code == -32602 ? "Invalid request parameters"
				      : code == -32000 ? "User declined request"
				      : code == -32800 ? "Request cancelled"
						       : "Client request failed";
		cJSON *error = cJSON_AddObjectToObject(response, "error");

		if (!error || !cJSON_AddNumberToObject(error, "code", code) ||
		    !cJSON_AddStringToObject(error, "message", message))
			return NB_ERR_NOMEM;
	}

	char *encoded NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(response);
	if (!encoded)
		return NB_ERR_NOMEM;

	*out = strdup(encoded);
	return *out ? 0 : NB_ERR_NOMEM;
}

static int subscription_uri_allowed(const cJSON *filters, const cJSON *uri)
{
	if (!cJSON_IsString(uri))
		return 0;

	const cJSON *uris = cJSON_GetObjectItemCaseSensitive(filters, "resourceSubscriptions");
	const cJSON *candidate;
	cJSON_ArrayForEach (candidate, uris) {
		if (cJSON_Compare(uri, candidate, 1))
			return 1;
	}

	return 0;
}

static int subscription_filter_allows(const cJSON *requested, const cJSON *accepted)
{
	const char *keys[] = {"toolsListChanged", "resourcesListChanged", "promptsListChanged"};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		const cJSON *value = cJSON_GetObjectItemCaseSensitive(accepted, keys[i]);
		if (value &&
		    (!cJSON_IsBool(value) ||
		     (cJSON_IsTrue(value) &&
		      !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(requested, keys[i])))))
			return 0;
	}

	const cJSON *uris = cJSON_GetObjectItemCaseSensitive(accepted, "resourceSubscriptions");
	if (uris && !cJSON_IsArray(uris))
		return 0;

	const cJSON *uri;
	cJSON_ArrayForEach (uri, uris) {
		if (!subscription_uri_allowed(requested, uri))
			return 0;
	}

	return 1;
}

int mcp_dispatch_message(struct mcp_client *client, const cJSON *message, char **reply)
{
	if (!client || !message || !reply)
		return NB_ERR_INVAL;

	if (*reply)
		return NB_ERR_EXISTS;

	const cJSON *version = cJSON_GetObjectItemCaseSensitive(message, "jsonrpc");
	const cJSON *method = cJSON_GetObjectItemCaseSensitive(message, "method");
	const cJSON *id = cJSON_GetObjectItemCaseSensitive(message, "id");
	const cJSON *params = cJSON_GetObjectItemCaseSensitive(message, "params");

	if (!cJSON_IsObject(message) || !cJSON_IsString(version) ||
	    strcmp(version->valuestring, "2.0") || !cJSON_IsString(method) ||
	    !method->valuestring[0] || cJSON_HasObjectItem(message, "result") ||
	    cJSON_HasObjectItem(message, "error")) {
		cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;
		return response_create(request_id_valid(id) ? id : NULL, NB_ERR_FORMAT, &result,
				       reply);
	}

	if (!id) {
		struct mcp_client *target =
			client->notification_target ? client->notification_target : client;
		const cJSON *meta = cJSON_GetObjectItemCaseSensitive(params, "_meta");
		int modern = client->protocol && client->protocol->per_request;
		const cJSON *subscription =
			modern ? cJSON_GetObjectItemCaseSensitive(
					 meta, "io.modelcontextprotocol/subscriptionId")
			       : NULL;
		int acknowledged = modern && !strcmp(method->valuestring,
						     "notifications/subscriptions/acknowledged");
		if (subscription) {
			if (!cJSON_IsNumber(subscription) || target->subscription_id <= 0 ||
			    subscription->valuedouble != target->subscription_id ||
			    (!acknowledged &&
			     !nb_atomic_int_load(&target->subscription_acknowledged)))
				return NB_ERR_PROTOCOL;
		} else if (acknowledged) {
			return NB_ERR_PROTOCOL;
		}

		if (acknowledged) {
			const cJSON *accepted =
				cJSON_GetObjectItemCaseSensitive(params, "notifications");
			if (!cJSON_IsObject(accepted) ||
			    !subscription_filter_allows(target->subscription_filters, accepted) ||
			    nb_atomic_int_load(&target->subscription_acknowledged))
				return NB_ERR_PROTOCOL;

			target->subscription_accepted = cJSON_Duplicate(accepted, 1);
			if (!target->subscription_accepted)
				return NB_ERR_NOMEM;

			target->subscription_acknowledged = 1;
		}

		if (subscription && !acknowledged) {
			const char *key =
				!strcmp(method->valuestring, "notifications/tools/list_changed")
					? "toolsListChanged"
				: !strcmp(method->valuestring,
					  "notifications/resources/list_changed")
					? "resourcesListChanged"
				: !strcmp(method->valuestring, "notifications/prompts/list_changed")
					? "promptsListChanged"
					: NULL;
			if (key && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
					   target->subscription_accepted, key)))
				return NB_ERR_PROTOCOL;

			if (!strcmp(method->valuestring, "notifications/resources/updated") &&
			    !subscription_uri_allowed(
				    target->subscription_accepted,
				    cJSON_GetObjectItemCaseSensitive(params, "uri")))
				return NB_ERR_PROTOCOL;
		}

		if (!strcmp(method->valuestring, "notifications/cancelled")) {
			const cJSON *request =
				cJSON_GetObjectItemCaseSensitive(params, "requestId");
			if (cJSON_IsNumber(request) &&
			    request->valuedouble == client->active_request_id)
				client->peer_cancelled = 1;

			if (cJSON_IsNumber(request) && target->subscription_id > 0 &&
			    request->valuedouble == target->subscription_id)
				target->subscription_ended = 1;
		}

		if (strcmp(method->valuestring, "notifications/tools/list_changed") == 0)
			target->tools_list_changed = 1;
		else if (strcmp(method->valuestring, "notifications/resources/list_changed") == 0)
			target->resources_list_changed = 1;
		else if (strcmp(method->valuestring, "notifications/prompts/list_changed") == 0)
			target->prompts_list_changed = 1;

		if (client->handlers.notification)
			client->handlers.notification(client->config.name, method->valuestring,
						      params, client->request_context,
						      client->handlers.user_data);

		return 0;
	}

	if (!request_id_valid(id)) {
		cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;
		return response_create(NULL, NB_ERR_FORMAT, &result, reply);
	}

	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int status =
		params && !cJSON_IsObject(params)
			? NB_ERR_INVAL
			: mcp_handle_input_request(client, method->valuestring, params, &result);

	return response_create(id, status, &result, reply);
}
